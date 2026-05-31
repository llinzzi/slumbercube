#include "audio_player_wrapper.h"
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

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static audio_stream_handle_t s_stream = NULL;
static audio_http_stream_handle_t s_http_stream = NULL;
static bool s_i2s_ready = false;
static bool s_mixer_ready = false;
static const char *s_status = NULL;

/* ── Radio config from /radio JSON API ─────────────────────── */
static char s_radio_url[256] = {0};
static char s_radio_station[64] = {0};
static char s_radio_song[128] = {0};
static int s_radio_volume_pct = -1;  /* -1 = not set, fallback to CONFIG */

#define RADIO_API_URL     "http://192.168.8.105:3000/radio"
#define STATUS_API_URL    "http://192.168.8.105:3000/api/status"

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

static esp_err_t audio_radio_fetch(void)
{
    char *resp_buf = malloc(1024);
    if (!resp_buf) return ESP_ERR_NO_MEM;

    int resp_len = 0;
    esp_http_client_config_t cfg = {
        .url = RADIO_API_URL,
        .timeout_ms = 5000,
        .buffer_size = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp_buf);
        ESP_LOGW(TAG, "Radio: HTTP init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Radio: HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(resp_buf);
        return err;
    }

    int ret = esp_http_client_fetch_headers(client);
    if (ret < 0 && ret != -1) {
        ESP_LOGW(TAG, "Radio: fetch headers failed");
        esp_http_client_cleanup(client);
        free(resp_buf);
        return ESP_FAIL;
    }

    /* Read response body */
    while (resp_len < 1023) {
        int r = esp_http_client_read(client, resp_buf + resp_len, 1023 - resp_len);
        if (r <= 0) break;
        resp_len += r;
    }
    resp_buf[resp_len] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || resp_len == 0) {
        ESP_LOGW(TAG, "Radio: HTTP %d, body=%d bytes", status, resp_len);
        free(resp_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Radio JSON: %s", resp_buf);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) {
        ESP_LOGW(TAG, "Radio: JSON parse failed");
        free(resp_buf);
        return ESP_FAIL;
    }

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

    cJSON_Delete(root);
    free(resp_buf);

    if (s_radio_url[0] == '\0') {
        ESP_LOGW(TAG, "Radio: no URL in response");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Radio: '%s' — '%s' -> %s",
             s_radio_station, s_radio_song, s_radio_url);
    return ESP_OK;
}

/* ── Poll /api/status for live song & volume ────────────────── */
void audio_poll_status(void)
{
    char *resp_buf = malloc(1024);
    if (!resp_buf) return;

    int resp_len = 0;
    esp_http_client_config_t cfg = {
        .url = STATUS_API_URL,
        .timeout_ms = 3000,
        .buffer_size = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buf); return; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(resp_buf);
        return;
    }

    int ret = esp_http_client_fetch_headers(client);
    if (ret < 0 && ret != -1) {
        esp_http_client_cleanup(client);
        free(resp_buf);
        return;
    }

    while (resp_len < 1023) {
        int r = esp_http_client_read(client, resp_buf + resp_len, 1023 - resp_len);
        if (r <= 0) break;
        resp_len += r;
    }
    resp_buf[resp_len] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || resp_len == 0) { free(resp_buf); return; }

    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) { free(resp_buf); return; }

    cJSON *j_song   = cJSON_GetObjectItem(root, "song");
    cJSON *j_volume = cJSON_GetObjectItem(root, "volume");

    if (j_song && cJSON_IsString(j_song) && j_song->valuestring[0]) {
        strncpy(s_radio_song, j_song->valuestring, sizeof(s_radio_song) - 1);
    }
    if (j_volume && cJSON_IsNumber(j_volume)) {
        double v = cJSON_GetNumberValue(j_volume);
        if (v >= 0.0 && v <= 1.0) {
            s_radio_volume_pct = (int)(v * 100.0 + 0.5);
        } else if (v > 1.0 && v <= 100.0) {
            s_radio_volume_pct = (int)(v + 0.5);
        }
        if (s_radio_volume_pct < 0)  s_radio_volume_pct = 0;
        if (s_radio_volume_pct > 100) s_radio_volume_pct = 100;
    }

    cJSON_Delete(root);
    free(resp_buf);
}

/* ══════════════════════════════════════════════════════════════
 * Audio playback
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_init(void)
{
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
    http_cfg.buffer_size          = 6 * 1024;
    http_cfg.high_watermark       = 4 * 1024;
    http_cfg.low_watermark        = 1 * 1024;
    http_cfg.task_stack_size      = 5 * 1024;
    http_cfg.read_timeout_ms      = 5000;
    http_cfg.reconnect_timeout_ms = 1000;
    http_cfg.enable_auto_reconnect = true;
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

    /* Wait for initial buffer — Icecast servers may burst-buffer for a few seconds
     * before sending, so allow up to 15s. 2KB is enough for MP3 format detection. */
    for (int wait = 0; wait < 150; wait++) {
        size_t buffered = audio_http_stream_get_buffered_bytes(s_http_stream);
        if (buffered >= 2048) {
            err = audio_stream_play_io(s_stream, io);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Playback started OK (%d bytes buffered)", (int)buffered);
            }
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timeout waiting for buffer (last: %d bytes)", (int)audio_http_stream_get_buffered_bytes(s_http_stream));
    audio_stop();
    s_status = "Network error";
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_play_url(const char *fallback_url)
{
    if (!s_i2s_ready) {
        ESP_LOGE(TAG, "I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_status = "Fetching radio...";

    /* Fetch radio config from /radio API; fall back to fallback_url on failure */
    if (audio_radio_fetch() != ESP_OK) {
        ESP_LOGW(TAG, "Radio fetch failed, using fallback URL");
        if (fallback_url && fallback_url[0]) {
            strncpy(s_radio_url, fallback_url, sizeof(s_radio_url) - 1);
        }
    }

    const char *play_url = s_radio_url[0] ? s_radio_url : fallback_url;
    if (!play_url || !play_url[0]) {
        ESP_LOGE(TAG, "No URL to play");
        return ESP_ERR_INVALID_STATE;
    }

    s_status = "Connecting...";

    if (audio_play_url_inner(play_url) == ESP_OK) {
        s_status = "Streaming...";
        return ESP_OK;
    }

    s_status = "Network error";
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_stop(void)
{
    if (s_stream) {
        audio_stream_stop(s_stream);
        audio_stream_delete(s_stream);
        s_stream = NULL;
    }
    if (s_http_stream) {
        audio_http_stream_close(s_http_stream);
        s_http_stream = NULL;
    }
    ESP_LOGI(TAG, "Playback stopped");
    return ESP_OK;
}

const char *audio_get_station_name(void)
{
    if (s_radio_song[0]) return s_radio_song;
    if (s_radio_station[0]) return s_radio_station;
    return s_status;
}

void audio_deinit(void)
{
    audio_stop();

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
