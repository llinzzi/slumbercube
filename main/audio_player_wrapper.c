#include "audio_player_wrapper.h"
#include "weather_service.h"
#include "wifi.h"
#include "agent_config.h"
#include "shtc3.h"
#include "audio_mixer.h"
#include "audio_stream.h"
#include "audio_http_stream.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "AUDIO";

/* Backend contract: the user-configurable host is spliced into a fixed
 * scheme / port / path prefix. Don't change these without coordinating
 * with the SlumberCube Agent server. */
#define RADIO_API_DEFAULT_HOST "192.168.8.192"
#define RADIO_API_PORT         3000
#define RADIO_API_PATH         "/api/esp"

/* Latest indoor temp/RH (NaN = sensor absent / not yet read).
 * Updated via audio_set_indoor_env() right before each /api/esp fetch. */
static float s_indoor_t = NAN;
static float s_indoor_h = NAN;

void audio_set_indoor_env(float temp_c, float humidity)
{
    s_indoor_t = temp_c;
    s_indoor_h = humidity;
}

/* Wake source for this boot cycle: "rtc", "btn", or NULL (cold boot).
 * Set once via audio_set_wake_source() before any network call. */
static const char *s_wake_source = NULL;

void audio_set_wake_source(const char *source)
{
    s_wake_source = source;
}

/* Agent (SlumberCube backend) config cache. Populated from NVS by
 * audio_agent_init() at the top of audio_init(); cleared by audio_deinit()
 * so the re-provisioning flow re-reads freshly-saved values. */
static agent_config_t s_agent = {
    .host    = RADIO_API_DEFAULT_HOST,
    .enabled = false,
};
static bool s_agent_loaded = false;

/* Read agent_cfg from NVS. On error or absent entry, leaves s_agent at
 * its compile-time defaults (host=192.168.8.192, enabled=false). */
static void audio_agent_init(void)
{
    agent_config_t loaded = {0};
    esp_err_t err = agent_config_load(&loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "agent_config_load failed (%s), defaulting to disabled",
                 esp_err_to_name(err));
        s_agent.host[0] = '\0';
        strncpy(s_agent.host, RADIO_API_DEFAULT_HOST, sizeof(s_agent.host) - 1);
        s_agent.host[sizeof(s_agent.host) - 1] = '\0';
        s_agent.enabled = false;
    } else {
        s_agent = loaded;
    }
    s_agent_loaded = true;
    ESP_LOGI(TAG, "Agent: %s, host='%s'",
             s_agent.enabled ? "enabled" : "disabled", s_agent.host);
}

esp_err_t audio_agent_reload(void)
{
    s_agent_loaded = false;
    audio_agent_init();
    return ESP_OK;
}

/* Query-string fragment "t=24.3&h=58" (no leading ? or &) — empty if no sensor.
 * Tries SHTC3 here as a last-chance read in case main.c didn't. */
static const char *indoor_query(void)
{
    static char qs[32] = {0};
    if (isnan(s_indoor_t)) {
        float t, h;
        if (shtc3_read(&t, &h)) {
            s_indoor_t = t;
            s_indoor_h = h;
        } else {
            s_indoor_t = NAN;  /* mark sensor absent, don't retry */
            s_indoor_h = NAN;
        }
    }
    if (isnan(s_indoor_t)) {
        qs[0] = '\0';
        return qs;
    }
    snprintf(qs, sizeof(qs), "t=%.1f&h=%.0f", s_indoor_t, s_indoor_h);
    return qs;
}

static const char *radio_api_url(void)
{
    static char url[384] = {0};
    static bool logged = false;

    if (!s_agent.enabled) {
        /* Caller must check via the audio_fetch_api() return code, not by
         * inspecting the URL. An empty buffer here is a sentinel. */
        if (!logged) {
            ESP_LOGI(TAG, "Agent disabled by config — /api/esp calls skipped");
            logged = true;
        }
        url[0] = '\0';
        return url;
    }

    char qs[96] = {0};
    int offset  = 0;

    /* Wake source is always the first query param (when present) */
    if (s_wake_source) {
        offset += snprintf(qs + offset, sizeof(qs) - offset,
                           "?wake=%s", s_wake_source);
    }

    /* Indoor temp/humidity (indoor_query returns "t=X&h=Y" or "") */
    const char *indoor = indoor_query();
    if (indoor[0]) {
        offset += snprintf(qs + offset, sizeof(qs) - offset,
                           "%c%s", (offset > 0) ? '&' : '?', indoor);
    }

    snprintf(url, sizeof(url), "http://%s:%d%s/%s%s",
             s_agent.host, RADIO_API_PORT, RADIO_API_PATH,
             wifi_get_device_id(), qs);

    if (!logged) {
        ESP_LOGI(TAG, "API URL: %s", url);
        logged = true;
    }
    return url;
}

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static audio_stream_handle_t s_stream = NULL;
static audio_http_stream_handle_t s_http_stream = NULL;
static bool s_i2s_ready = false;
static bool s_mixer_ready = false;
static bool s_playback_active = false;  /* true only when playback actually started */
static const char *s_status = NULL;
static int s_content_length = 0;

/* ── Radio config from /radio JSON API ─────────────────────── */
static char s_radio_url[256] = {0};
static char s_radio_station[64] = {0};
static char s_radio_song[128] = {0};
static int s_radio_volume_pct = -1;  /* -1 = not set, fallback to CONFIG */
static weather_data_t s_cached_weather = {0};
static audio_alarm_config_t s_alarm_config = {0};


/* ── Software volume scale (16-bit stereo PCM) ──────────────── */
static void apply_volume(void *buf, size_t len)
{
    int vol = (s_radio_volume_pct >= 0) ? s_radio_volume_pct : CONFIG_AUDIO_VOLUME_PCT;
    if (vol >= 100) return;

    int16_t *samples = (int16_t *)buf;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)((int32_t)samples[i] * vol / 100);
    }
}

/* ── I2S write callback for mixer ─────────────────────────── */
static esp_err_t i2s_write(void *audio_buffer, size_t len,
                           size_t *bytes_written, uint32_t timeout_ms)
{
    apply_volume(audio_buffer, len);
    /* Cap timeout to prevent watchdog reset if I2S stalls */
    if (timeout_ms > 100) timeout_ms = 100;
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan, audio_buffer, len,
                                      bytes_written, timeout_ms);
    if (*bytes_written == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return err;
}

/* ── I2S clock reconfig for mixer ─────────────────────────── */
static uint32_t s_last_rate = 0;
static uint32_t s_last_bits = 0;
static uint32_t s_last_ch = 0;

static esp_err_t i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg,
                                  i2s_slot_mode_t ch)
{
    /* Skip reconfig if format hasn't changed (avoids I2S glitches) */
    uint32_t channel_count = (ch == I2S_SLOT_MODE_STEREO) ? 2 : 1;
    if (rate == s_last_rate && bits_cfg == s_last_bits && channel_count == s_last_ch) {
        return ESP_OK;
    }
    s_last_rate = rate;
    s_last_bits = bits_cfg;
    s_last_ch = channel_count;

    i2s_channel_disable(s_i2s_tx_chan);
    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = rate,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };
    esp_err_t err = i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Clock reconfig failed for %lu Hz, keeping old rate", rate);
        /* Restore old rate in mixer config */
        s_last_rate = 0; /* force reconfig next time */
    }
    i2s_channel_enable(s_i2s_tx_chan);
    return ESP_OK; /* Don't block playback on clock error */
}

/* ── Mute/unmute: control NS4168 CTL pin ──────────────────── */
static esp_err_t mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    int level = (setting == AUDIO_PLAYER_UNMUTE) ? 1 : 0;
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, level);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Radio JSON API
 * ══════════════════════════════════════════════════════════════ */

/* Fetch /api/esp and return parsed JSON. Caller must cJSON_Delete(root).
 * Retries once after a 2s delay on transient network errors. */
static esp_err_t audio_http_get_json(cJSON **out_root)
{
    *out_root = NULL;

    if (!s_agent.enabled) {
        /* Sentinel: the agent is configured off. Caller handles the
         * ESP_ERR_NOT_SUPPORTED return code (audio_fetch_api propagates it
         * to main.c, which skips weather + radio display). */
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Radio: retrying after 2s delay");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        static char resp_buf[2048];
        int resp_len = 0;
        esp_http_client_config_t cfg = {
            .url = radio_api_url(),
            .timeout_ms = 10000,
            .buffer_size = 512,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGW(TAG, "Radio: HTTP init failed");
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Radio: HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        int ret = esp_http_client_fetch_headers(client);
        if (ret < 0 && ret != -1) {
            ESP_LOGW(TAG, "Radio: fetch headers failed");
            esp_http_client_cleanup(client);
            continue;
        }

        while (resp_len < 2047) {
            int r = esp_http_client_read(client, resp_buf + resp_len, 2047 - resp_len);
            if (r <= 0) break;
            resp_len += r;
        }
        resp_buf[resp_len] = '\0';

        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (status != 200 || resp_len == 0) {
            ESP_LOGW(TAG, "Radio: HTTP %d, body=%d bytes", status, resp_len);
            continue;
        }

        *out_root = cJSON_Parse(resp_buf);
        if (!*out_root) {
            ESP_LOGW(TAG, "Radio: JSON parse failed");
            continue;
        }
        return ESP_OK;
    }

    return ESP_FAIL;
}

/* Parse only weather from /api/esp JSON. Stores result in s_cached_weather.
 * Does NOT touch radio state — safe to call from audio_fetch_api(). */
static void audio_parse_weather(cJSON *root)
{
    cJSON *j_weather = cJSON_GetObjectItem(root, "weather");
    if (j_weather && cJSON_IsObject(j_weather)) {
        weather_data_t w = {0};
        daily_forecast_t *d = &w.daily[0];

        cJSON *j_temp   = cJSON_GetObjectItem(j_weather, "temp");
        cJSON *j_text   = cJSON_GetObjectItem(j_weather, "text");
        cJSON *j_humid  = cJSON_GetObjectItem(j_weather, "humidity");
        cJSON *j_tmax   = cJSON_GetObjectItem(j_weather, "tempMax");
        cJSON *j_tmin   = cJSON_GetObjectItem(j_weather, "tempMin");
        cJSON *j_tday   = cJSON_GetObjectItem(j_weather, "textDay");
        cJSON *j_tnight = cJSON_GetObjectItem(j_weather, "textNight");

        d->current  = (j_temp  && cJSON_IsString(j_temp))  ? atoi(j_temp->valuestring)  : 0;
        d->high     = (j_tmax  && cJSON_IsString(j_tmax))  ? atoi(j_tmax->valuestring)  : 0;
        d->low      = (j_tmin  && cJSON_IsString(j_tmin))  ? atoi(j_tmin->valuestring)  : 0;
        d->humidity = (j_humid && cJSON_IsString(j_humid)) ? atoi(j_humid->valuestring) : 0;

        if (j_text && cJSON_IsString(j_text)) {
            strncpy(d->current_text, j_text->valuestring, sizeof(d->current_text) - 1);
        }
        if (j_tday && cJSON_IsString(j_tday)) {
            strncpy(d->day_text, j_tday->valuestring, sizeof(d->day_text) - 1);
        }
        if (j_tnight && cJSON_IsString(j_tnight)) {
            strncpy(d->night_text, j_tnight->valuestring, sizeof(d->night_text) - 1);
        }

        struct tm tm_now = {0};
        time_t now = time(NULL);
        localtime_r(&now, &tm_now);
        d->month = tm_now.tm_mon + 1;
        d->day   = tm_now.tm_mday;

        w.count = 1;
        w.valid = true;
        w.fetch_time = now;
        s_cached_weather = w;

        ESP_LOGI(TAG, "Weather: %s %d°  hi=%d° lo=%d°  humid=%d%%",
                 d->current_text, d->current, d->high, d->low, d->humidity);
    } else {
        ESP_LOGW(TAG, "No weather block in /api/esp");
    }
}

/* Parse radio fields (url/name/song/volume) from /api/esp JSON.
 * Clears previous radio state FIRST, then populates from response.
 * When a field is missing in the new response, it stays empty — no
 * stale data from a previous fetch persists. */
static void audio_parse_radio(cJSON *root)
{
    /* ── Reset radio state ── */
    s_radio_url[0]      = '\0';
    s_radio_station[0]  = '\0';
    s_radio_song[0]     = '\0';
    s_radio_volume_pct  = -1;

    /* ── Radio URL / station / song / volume ── */
    cJSON *j_url    = cJSON_GetObjectItem(root, "url");
    cJSON *j_name   = cJSON_GetObjectItem(root, "name");
    cJSON *j_song   = cJSON_GetObjectItem(root, "song");
    cJSON *j_volume = cJSON_GetObjectItem(root, "volume");

    if (j_url && cJSON_IsString(j_url) && j_url->valuestring[0]) {
        strncpy(s_radio_url, j_url->valuestring, sizeof(s_radio_url) - 1);
        s_radio_url[sizeof(s_radio_url) - 1] = '\0';
    }
    if (j_name && cJSON_IsString(j_name)) {
        strncpy(s_radio_station, j_name->valuestring, sizeof(s_radio_station) - 1);
    }
    if (j_song && cJSON_IsString(j_song)) {
        strncpy(s_radio_song, j_song->valuestring, sizeof(s_radio_song) - 1);
    }
    if (j_volume && cJSON_IsNumber(j_volume)) {
        double v = cJSON_GetNumberValue(j_volume);
        /* Support both 0.0-1.0 (float) and 0-100 (integer/percentage) */
        if (v >= 0.0 && v <= 1.0) {
            s_radio_volume_pct = (int)(v * 100.0 + 0.5);
        } else if (v > 1.0 && v <= 100.0) {
            s_radio_volume_pct = (int)(v + 0.5);
        } else {
            s_radio_volume_pct = (int)v;
        }
        if (s_radio_volume_pct < 0)  s_radio_volume_pct = 0;
        if (s_radio_volume_pct > 100) s_radio_volume_pct = 100;
        ESP_LOGI(TAG, "Radio: volume=%.2f -> %d%%", v, s_radio_volume_pct);
    }
}

/* Parse alarm field from /api/esp JSON.
 * Expected format: {"alarm":{"enabled":true,"time":"07:50",...}}
 *
 * Three terminal states:
 *   - alarm object missing / malformed  → valid=false, disabled=false
 *   - enabled=false                     → valid=false, disabled=true
 *                                         (caller skips PCF85063 arm + internal timer)
 *   - enabled=true + valid time/weekend → valid=true,  disabled=false
 */
static void audio_parse_alarm(cJSON *root)
{
    s_alarm_config.valid    = false;
    s_alarm_config.disabled = false;

    cJSON *j_alarm = cJSON_GetObjectItem(root, "alarm");
    if (!j_alarm || !cJSON_IsObject(j_alarm)) return;

    cJSON *j_enabled = cJSON_GetObjectItem(j_alarm, "enabled");
    if (!j_enabled || !cJSON_IsBool(j_enabled)) return;

    /* Server explicitly disabled the alarm — record and bail. */
    if (cJSON_IsFalse(j_enabled)) {
        s_alarm_config.disabled = true;
        ESP_LOGI(TAG, "Alarm: server disabled");
        return;
    }
    if (!cJSON_IsTrue(j_enabled)) return;

    /* "time": "HH:MM" */
    cJSON *j_time = cJSON_GetObjectItem(j_alarm, "time");
    if (!j_time || !cJSON_IsString(j_time)) return;

    const char *s = j_time->valuestring;
    int h = 0, m = 0;
    if (sscanf(s, "%2d:%2d", &h, &m) != 2) return;
    if (h < 0 || h > 23 || m < 0 || m > 59) return;

    s_alarm_config.hour             = (uint8_t)h;
    s_alarm_config.minute           = (uint8_t)m;
    s_alarm_config.valid            = true;

    /* Weekend flags — default to false (skip weekends) if missing. */
    cJSON *j_sat = cJSON_GetObjectItem(j_alarm, "weekend_saturday");
    cJSON *j_sun = cJSON_GetObjectItem(j_alarm, "weekend_sunday");
    s_alarm_config.weekend_saturday = (j_sat && cJSON_IsBool(j_sat) &&
                                       cJSON_IsTrue(j_sat));
    s_alarm_config.weekend_sunday   = (j_sun && cJSON_IsBool(j_sun) &&
                                       cJSON_IsTrue(j_sun));

    ESP_LOGI(TAG, "Alarm: %02d:%02d (sat=%d sun=%d)",
             h, m, s_alarm_config.weekend_saturday,
             s_alarm_config.weekend_sunday);
}

const audio_alarm_config_t *audio_get_alarm_config(void)
{
    return &s_alarm_config;
}

/* Fetch /api/esp and parse weather + radio fields.
 * Used as the boot weather fetch — also caches s_radio_url so
 * audio_play_url() can skip a redundant HTTP round-trip. */
esp_err_t audio_fetch_api(void)
{
    cJSON *root = NULL;
    esp_err_t err = audio_http_get_json(&root);
    if (err != ESP_OK) return err;

    audio_parse_weather(root);
    audio_parse_radio(root);
    audio_parse_alarm(root);
    cJSON_Delete(root);

    /* Success if we got usable weather data */
    return s_cached_weather.valid ? ESP_OK : ESP_FAIL;
}

/* Fetch /api/esp for a fresh radio URL (auto-advance, button toggle).
 * Always clears previous radio state before parsing. */
static esp_err_t audio_radio_fetch(void)
{
    cJSON *root = NULL;
    esp_err_t err = audio_http_get_json(&root);
    if (err != ESP_OK) return err;

    audio_parse_radio(root);
    audio_parse_weather(root);
    cJSON_Delete(root);

    if (s_radio_url[0] == '\0') {
        ESP_LOGW(TAG, "Radio: no URL in response");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Radio: '%s' — '%s' -> %s",
             s_radio_station, s_radio_song, s_radio_url);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Audio playback
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_init(void)
{
    /* Load agent config on the first call per boot. Cleared by
     * audio_deinit() so the re-provisioning flow re-reads NVS. */
    if (!s_agent_loaded) {
        audio_agent_init();
    }
    if (s_i2s_ready) return ESP_OK;

    ESP_LOGI(TAG, "Init I2S (SDIN=%d SCLK=%d WS=%d)",
             CONFIG_PIN_I2S_SDIN, CONFIG_PIN_I2S_SCLK, CONFIG_PIN_I2S_LROUT);

    /* Power on NS4168 and let it settle */
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    /* Create I2S TX channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* Standard Philips mode: 44.1kHz / 16-bit / stereo */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_PIN_I2S_SCLK,
            .ws   = CONFIG_PIN_I2S_LROUT,
            .dout = CONFIG_PIN_I2S_SDIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    s_i2s_ready = true;
    ESP_LOGI(TAG, "I2S initialized OK");
    return ESP_OK;
}

static esp_err_t audio_play_url_inner(const char *url)
{
    /* Stop any existing playback first */
    audio_stop();

    s_status = "Connecting...";
    ESP_LOGI(TAG, "Trying: %s", url);

    /* Init mixer (safe to call repeatedly — no-op if already running) */
    audio_mixer_config_t mixer_cfg = {
        .mute_fn    = mute_fn,
        .clk_set_fn = i2s_reconfig_clk,
        .write_fn   = i2s_write,
        .priority   = 5,
        .coreID     = tskNO_AFFINITY,
        .i2s_format = {
            .sample_rate     = 44100,
            .bits_per_sample = 16,
            .channels        = 2,
        },
    };
    esp_err_t err = audio_mixer_init(&mixer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mixer_init: %s", esp_err_to_name(err));
        return err;
    }
    s_mixer_ready = true;

    /* Create decoder stream */
    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("music");
    s_stream = audio_stream_new(&stream_cfg);
    if (!s_stream) {
        ESP_LOGE(TAG, "stream_new failed");
        return ESP_FAIL;
    }

    /* Open HTTP stream */
    audio_http_stream_config_t http_cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG(url);
    http_cfg.buffer_size          = 4 * 1024;
    http_cfg.high_watermark       = 3 * 1024;
    http_cfg.low_watermark        = 1 * 1024;
    http_cfg.task_stack_size      = 6 * 1024;
    http_cfg.task_priority        = 6;   // above mixer/decoder (5) so the socket
                                         // connect/read isn't starved of CPU
    http_cfg.read_timeout_ms      = 30000;  // /audio/local/track can wait for intro buffer
    http_cfg.reconnect_timeout_ms = 1500;
    http_cfg.enable_auto_reconnect = false;
    s_http_stream = audio_http_stream_open(&http_cfg);
    if (!s_http_stream) {
        ESP_LOGE(TAG, "http_stream_open failed");
        return ESP_FAIL;
    }

    audio_stream_io_handle_t io;
    err = audio_http_stream_get_io(s_http_stream, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_stream_get_io: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for initial buffer, then try to start playback.
     * Retry with fallback sample rates if mixer rejects the stream's rate. */
    static const uint32_t RATES[] = {44100, 16000, 22050, 48000};
    for (int wait = 0; wait < 300; wait++) {
        size_t buffered = audio_http_stream_get_buffered_bytes(s_http_stream);
        if (buffered >= 2048) {
            for (int ri = 0; ri < (int)(sizeof(RATES)/sizeof(RATES[0])); ri++) {
                if (ri > 0) {
                    /* Reinit mixer at new sample rate */
                    audio_mixer_deinit();
                    mixer_cfg.i2s_format.sample_rate = RATES[ri];
                    if (audio_mixer_init(&mixer_cfg) != ESP_OK) continue;
                    /* Recreate stream */
                    audio_stream_stop(s_stream);
                    audio_stream_delete(s_stream);
                    s_stream = audio_stream_new(&stream_cfg);
                    if (!s_stream) break;
                }
                err = audio_stream_play_io(s_stream, io);
                if (err == ESP_OK) {
                    s_playback_active = true;
                    s_content_length = audio_http_stream_get_content_length(s_http_stream);
                    ESP_LOGI(TAG, "Playback started at %lu Hz (%d bytes buffered)", RATES[ri], (int)buffered);
                    return ESP_OK;
                }
                ESP_LOGW(TAG, "play_io failed at %lu Hz: %s", RATES[ri], esp_err_to_name(err));
            }
            return err;  /* all rates failed */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timeout waiting for buffer (last: %d bytes)", (int)audio_http_stream_get_buffered_bytes(s_http_stream));
    audio_stop();
    s_status = "Network error";
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_play_url(void)
{
    if (!s_i2s_ready) {
        ESP_LOGE(TAG, "I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_agent.enabled) {
        ESP_LOGD(TAG, "audio_play_url: agent disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_status = "Fetching radio...";

    /* Fetch radio config from /api/esp unless already cached by
     * audio_fetch_api() during boot — avoids a double HTTP round-trip. */
    if (s_radio_url[0] == '\0') {
        if (audio_radio_fetch() != ESP_OK) {
            ESP_LOGW(TAG, "Radio fetch failed, no URL to play");
            return ESP_FAIL;
        }
    }

    const char *play_url = s_radio_url;
    if (!play_url || !play_url[0]) {
        ESP_LOGE(TAG, "No URL to play");
        return ESP_ERR_INVALID_STATE;
    }

    /* Copy URL before calling audio_play_url_inner() — it calls
     * audio_stop() which clears s_radio_url, invalidating the pointer. */
    char url_copy[256];
    strncpy(url_copy, play_url, sizeof(url_copy) - 1);
    url_copy[sizeof(url_copy) - 1] = '\0';

    s_status = "Connecting...";

    if (audio_play_url_inner(url_copy) == ESP_OK) {
        s_status = "Streaming...";
        return ESP_OK;
    }

    s_status = "Network error";
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_stop(void)
{
    /* Close HTTP stream first — stops download task from filling the ringbuf.
     * This unblocks the drain in audio_http_stream_close() and lets the task
     * exit cleanly before we delete the decoder that was consuming the data. */
    if (s_http_stream) {
        audio_http_stream_close(s_http_stream);
        s_http_stream = NULL;
    }
    if (s_stream) {
        audio_stream_stop(s_stream);
        audio_stream_delete(s_stream);
        s_stream = NULL;
    }
    s_playback_active = false;
    s_content_length = 0;
    s_radio_url[0] = '\0';  /* force re-fetch on next play */
    ESP_LOGI(TAG, "Playback stopped");
    return ESP_OK;
}

const char *audio_get_station_name(void)
{
    if (s_radio_song[0]) return s_radio_song;
    if (s_radio_station[0]) return s_radio_station;
    return s_status;
}

bool audio_is_finished(void)
{
    if (!s_playback_active) return false;  /* never started or already handled */
    if (!s_stream) return true;
    audio_player_state_t st = audio_stream_get_state(s_stream);
    ESP_LOGD(TAG, "stream state: %d", (int)st);
    return st == AUDIO_PLAYER_STATE_IDLE || st == AUDIO_PLAYER_STATE_SHUTDOWN;
}

bool audio_radio_url_is_set(void)
{
    return s_radio_url[0] != '\0';
}

int audio_get_progress(void)
{
    if (!s_http_stream || s_content_length <= 0) return -1;
    size_t downloaded = audio_http_stream_get_total_bytes(s_http_stream);
    int pct = (int)((downloaded * 100) / (size_t)s_content_length);
    if (pct > 100) pct = 100;
    return pct;
}

const weather_data_t *audio_get_weather(void)
{
    return &s_cached_weather;
}

void audio_deinit(void)
{
    audio_stop();

    /* Force the next audio_init() to re-read agent_cfg from NVS — the user
     * may have just re-provisioned with a new host. */
    s_agent_loaded = false;

    if (s_mixer_ready) {
        audio_mixer_deinit();
        s_mixer_ready = false;
    }

    if (s_i2s_tx_chan) {
        i2s_channel_disable(s_i2s_tx_chan);
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }

    /* Shut down NS4168 for power saving */
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);
    s_i2s_ready = false;

    ESP_LOGI(TAG, "Deinitialized");
}
