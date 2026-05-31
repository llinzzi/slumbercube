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
#include <stdlib.h>

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static audio_stream_handle_t s_stream = NULL;
static audio_http_stream_handle_t s_http_stream = NULL;
static bool s_i2s_ready = false;
static bool s_mixer_ready = false;

/* ── Playlist state ──────────────────────────────────────────── */
#define MAX_TRACKS 80

typedef struct {
    char *url;
    char *name;
} track_t;

static track_t *s_tracks = NULL;
static int s_track_count = 0;
static int s_current_track = -1;
static int s_track_retry = 0;
static int s_track_connect_wait = 0;
static char s_playlist_url[256] = {0};
static char s_radio_name[64] = {0};
static volatile bool s_track_done = false;
static bool s_track_pending = false;  /* play_track_url called, waiting for buffer */
static const char *s_pending_url = NULL;

static int s_radio_volume_pct = -1;  /* -1 = not set, fallback to CONFIG */

#define RADIO_API_URL "http://192.168.8.105:3000/radio"

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
        ESP_LOGW(TAG, "Clock reconfig failed for %lu Hz", rate);
        s_last_rate = 0;
    }
    i2s_channel_enable(s_i2s_tx_chan);
    return ESP_OK;
}

/* ── Mute/unmute: control NS4168 CTL pin ──────────────────── */
static esp_err_t mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    int level = (setting == AUDIO_PLAYER_UNMUTE) ? 1 : 0;
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, level);
    return ESP_OK;
}

/* ── Mixer callback: detect track end ─────────────────────── */
static void mixer_callback(audio_player_cb_ctx_t *ctx)
{
    if (ctx && ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        s_track_done = true;
    }
}

/* ══════════════════════════════════════════════════════════════
 * Radio JSON API
 * ══════════════════════════════════════════════════════════════ */

static esp_err_t radio_fetch(void)
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
    if (!client) { free(resp_buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(resp_buf);
        return err;
    }

    esp_http_client_fetch_headers(client);
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
        ESP_LOGW(TAG, "Radio: HTTP %d, %d bytes", status, resp_len);
        free(resp_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Radio JSON: %s", resp_buf);
    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) { free(resp_buf); return ESP_FAIL; }

    cJSON *j_playlist = cJSON_GetObjectItem(root, "playlist");
    cJSON *j_name     = cJSON_GetObjectItem(root, "name");
    cJSON *j_volume   = cJSON_GetObjectItem(root, "volume");

    if (j_playlist && cJSON_IsString(j_playlist)) {
        strncpy(s_playlist_url, j_playlist->valuestring, sizeof(s_playlist_url) - 1);
    }
    if (j_name && cJSON_IsString(j_name)) {
        strncpy(s_radio_name, j_name->valuestring, sizeof(s_radio_name) - 1);
    }
    if (j_volume && cJSON_IsNumber(j_volume)) {
        double v = cJSON_GetNumberValue(j_volume);
        if (v >= 0.0 && v <= 1.0)      s_radio_volume_pct = (int)(v * 100.0 + 0.5);
        else if (v > 1.0 && v <= 100.0) s_radio_volume_pct = (int)(v + 0.5);
        else                            s_radio_volume_pct = (int)v;
        if (s_radio_volume_pct < 0)  s_radio_volume_pct = 0;
        if (s_radio_volume_pct > 100) s_radio_volume_pct = 100;
        ESP_LOGI(TAG, "Radio: volume=%.2f -> %d%%", v, s_radio_volume_pct);
    }

    cJSON_Delete(root);
    free(resp_buf);

    if (s_playlist_url[0] == '\0') {
        ESP_LOGW(TAG, "Radio: no playlist URL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Radio: '%s' playlist=%s", s_radio_name, s_playlist_url);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * M3U Playlist
 * ══════════════════════════════════════════════════════════════ */

/*
 * Parse #EXTINF line: "#EXTINF:292,🎵 宫崎骏-出发 (魔女宅急便 水晶音乐)"
 * Extract everything after "🎵 " (or after ",🎵" then skip the space).
 */
static void parse_extinf(const char *line, char *name_out, size_t name_size)
{
    name_out[0] = '\0';
    const char *p = strchr(line, ',');
    if (!p) return;
    p++;  /* skip comma */
    /* Skip "🎵 " prefix (4 UTF-8 bytes + space = 5 chars) */
    if ((unsigned char)p[0] == 0xF0 && (unsigned char)p[1] == 0x9F &&
        (unsigned char)p[2] == 0x8E && (unsigned char)p[3] == 0xB5) {
        p += 4;
        if (*p == ' ') p++;
    }
    strncpy(name_out, p, name_size - 1);
    name_out[name_size - 1] = '\0';
}

static esp_err_t playlist_fetch(void)
{
    if (s_playlist_url[0] == '\0') {
        ESP_LOGW(TAG, "No playlist URL");
        return ESP_FAIL;
    }

    char *resp_buf = malloc(12288);
    if (!resp_buf) return ESP_ERR_NO_MEM;

    int resp_len = 0;
    esp_http_client_config_t cfg = {
        .url = s_playlist_url,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(resp_buf);
        return err;
    }

    esp_http_client_fetch_headers(client);
    while (resp_len < 12287) {
        int r = esp_http_client_read(client, resp_buf + resp_len, 12287 - resp_len);
        if (r <= 0) break;
        resp_len += r;
    }
    resp_buf[resp_len] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (resp_len == 0) {
        ESP_LOGW(TAG, "M3U empty");
        free(resp_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "M3U: %d bytes", resp_len);

    /* Allocate track array (dynamic to save BSS) */
    if (s_tracks) {
        for (int i = 0; i < s_track_count; i++) {
            free(s_tracks[i].url);
            free(s_tracks[i].name);
        }
        free(s_tracks);
    }
    s_tracks = calloc(MAX_TRACKS, sizeof(track_t));
    if (!s_tracks) { free(resp_buf); return ESP_ERR_NO_MEM; }
    s_track_count = 0;

    /* Parse M3U lines */
    char *saveptr;
    char *line = strtok_r(resp_buf, "\r\n", &saveptr);
    char pending_name[128] = {0};

    while (line && s_track_count < MAX_TRACKS) {
        if (line[0] == '\0' || (line[0] == '#' && strncmp(line, "#EXTINF:", 8) != 0)) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            parse_extinf(line, pending_name, sizeof(pending_name));
        } else if (line[0] == 'h' && strncmp(line, "http", 4) == 0) {
            s_tracks[s_track_count].url = strdup(line);
            if (pending_name[0]) {
                s_tracks[s_track_count].name = strdup(pending_name);
            } else {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "Track %d", s_track_count + 1);
                s_tracks[s_track_count].name = strdup(tmp);
            }
            ESP_LOGI(TAG, "  [%d] '%s'", s_track_count, s_tracks[s_track_count].name);
            s_track_count++;
            pending_name[0] = '\0';
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(resp_buf);

    if (s_track_count == 0) {
        ESP_LOGW(TAG, "M3U: no tracks found");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "M3U: %d tracks", s_track_count);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Audio playback — single track
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_init(void)
{
    if (s_i2s_ready) return ESP_OK;

    ESP_LOGI(TAG, "Init I2S (SDIN=%d SCLK=%d WS=%d)",
             CONFIG_PIN_I2S_SDIN, CONFIG_PIN_I2S_SCLK, CONFIG_PIN_I2S_LROUT);

    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err)); return err; }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_init_std_mode: %s", esp_err_to_name(err)); return err; }

    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err)); return err; }

    s_i2s_ready = true;
    ESP_LOGI(TAG, "I2S initialized OK");
    return ESP_OK;
}

static esp_err_t play_track_url(const char *url)
{
    audio_stop();

    ESP_LOGI(TAG, "Track: %s", url);

    audio_mixer_config_t mixer_cfg = {
        .mute_fn    = mute_fn,
        .clk_set_fn = i2s_reconfig_clk,
        .write_fn   = i2s_write,
        .priority   = 5,
        .coreID     = tskNO_AFFINITY,
        .i2s_format = { .sample_rate = 44100, .bits_per_sample = 16, .channels = 2 },
    };
    esp_err_t err = audio_mixer_init(&mixer_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mixer_init: %s", esp_err_to_name(err)); return err; }
    s_mixer_ready = true;

    /* Register callback to detect track end */
    audio_mixer_callback_register(mixer_callback);

    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("music");
    s_stream = audio_stream_new(&stream_cfg);
    if (!s_stream) { ESP_LOGE(TAG, "stream_new failed"); return ESP_FAIL; }

    audio_http_stream_config_t http_cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG(url);
    http_cfg.buffer_size          = 6 * 1024;
    http_cfg.high_watermark       = 4 * 1024;
    http_cfg.low_watermark        = 1 * 1024;
    http_cfg.task_stack_size      = 5 * 1024;
    http_cfg.task_priority        = 7;
    http_cfg.read_timeout_ms      = 15000;
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
        audio_stop();
        return err;
    }

    /* Non-blocking: return immediately. audio_service() will poll for buffer. */
    s_track_done = false;
    s_track_pending = true;
    s_track_connect_wait = 0;
    s_pending_url = url;
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Playlist player
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_play_radio(void)
{
    if (!s_i2s_ready) {
        ESP_LOGE(TAG, "I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Fetch /radio JSON to get playlist URL (only on first call or replay) */
    if (s_track_count == 0 || s_current_track >= s_track_count) {
        ESP_LOGI(TAG, "Fetching /radio...");
        if (radio_fetch() != ESP_OK) {
            ESP_LOGW(TAG, "/radio failed");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Fetching playlist...");
        if (playlist_fetch() != ESP_OK) {
            ESP_LOGW(TAG, "Playlist fetch failed");
            return ESP_FAIL;
        }
        s_current_track = 0;
    }

    return play_track_url(s_tracks[s_current_track].url);
}

esp_err_t audio_play_url(const char *url)
{
    if (!s_i2s_ready) return ESP_ERR_INVALID_STATE;
    /* Legacy: play a single URL directly */
    return play_track_url(url);
}

void audio_service(void)
{
    /* Track playing normally — check for EOF */
    if (!s_track_done && s_http_stream && !s_track_pending) {
        return;
    }

    /* Waiting for buffer to fill after play_track_url() */
    if (s_track_pending && s_http_stream) {
        size_t buffered = audio_http_stream_get_buffered_bytes(s_http_stream);
        if (buffered >= 2048) {
            audio_stream_io_handle_t io;
            if (audio_http_stream_get_io(s_http_stream, &io) == ESP_OK) {
                esp_err_t err = audio_stream_play_io(s_stream, io);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Track started (%d bytes buffered)", (int)buffered);
                    s_track_pending = false;
                    s_track_retry = 0;
                }
            }
            return;
        }
        s_track_connect_wait++;
        if (s_track_connect_wait > 25) {
            ESP_LOGW(TAG, "Track connect timeout (%d bytes buffered)", (int)buffered);
            s_track_pending = false;
            audio_stop();
        }
        return;
    }

    /* Track ended (EOF from mixer callback) */
    if (s_track_done) {
        s_track_done = false;
        s_track_pending = false;
        audio_stop();
        s_current_track++;
        vTaskDelay(pdMS_TO_TICKS(300));
    } else if (!s_http_stream && !s_track_pending) {
        /* Connection failed before any data arrived */
        if (s_track_retry < 1) {
            s_track_retry++;
            ESP_LOGW(TAG, "Retry [%d/%d]: '%s'", s_current_track + 1, s_track_count,
                     s_tracks[s_current_track].name);
        } else {
            ESP_LOGW(TAG, "Track failed, skipping");
            s_track_retry = 0;
            s_current_track++;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    } else {
        return;
    }

    /* Check if playlist exhausted */
    if (s_current_track >= s_track_count) {
        ESP_LOGI(TAG, "Playlist done, re-fetching...");
        if (radio_fetch() == ESP_OK && playlist_fetch() == ESP_OK) {
            s_current_track = 0;
        } else {
            s_current_track = 0;
        }
    }

    ESP_LOGI(TAG, "Next [%d/%d]: '%s'", s_current_track + 1, s_track_count,
             s_tracks[s_current_track].name);
    play_track_url(s_tracks[s_current_track].url);
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
    return ESP_OK;
}

const char *audio_get_station_name(void)
{
    /* Show current track name from playlist */
    if (s_current_track >= 0 && s_current_track < s_track_count) {
        return s_tracks[s_current_track].name;
    }
    if (s_radio_name[0]) return s_radio_name;
    return NULL;
}

bool audio_is_playing(void)
{
    return s_http_stream != NULL && s_current_track >= 0;
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

    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);
    s_i2s_ready = false;
    ESP_LOGI(TAG, "Deinitialized");
}
