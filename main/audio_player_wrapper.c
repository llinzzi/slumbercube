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
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static audio_stream_handle_t s_stream = NULL;
static bool s_i2s_ready = false;
static bool s_mixer_ready = false;
static volatile bool s_track_done = false;

/* ── Playlist state ──────────────────────────────────────────── */
#define MAX_TRACKS 80

typedef struct {
    char *url;
    char *name;
} track_t;

static track_t *s_tracks = NULL;
static int s_track_count = 0;
static int s_current_track = -1;
static char s_playlist_url[256] = {0};
static char s_radio_name[64] = {0};

static int s_radio_volume_pct = -1;

#define RADIO_API_URL "http://192.168.8.105:3000/radio"

/* ── Software volume scale ──────────────────────────────────── */
static void apply_volume(void *buf, size_t len)
{
    int vol = (s_radio_volume_pct >= 0) ? s_radio_volume_pct : CONFIG_AUDIO_VOLUME_PCT;
    if (vol >= 100) return;
    int16_t *samples = (int16_t *)buf;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++)
        samples[i] = (int16_t)((int32_t)samples[i] * vol / 100);
}

/* ── I2S write callback ─────────────────────────────────────── */
static esp_err_t i2s_write(void *audio_buffer, size_t len,
                           size_t *bytes_written, uint32_t timeout_ms)
{
    apply_volume(audio_buffer, len);
    if (timeout_ms > 100) timeout_ms = 100;
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan, audio_buffer, len,
                                      bytes_written, timeout_ms);
    if (*bytes_written == 0) vTaskDelay(pdMS_TO_TICKS(50));
    return err;
}

/* ── I2S clock reconfig ─────────────────────────────────────── */
static uint32_t s_last_rate = 0, s_last_bits = 0, s_last_ch = 0;

static esp_err_t i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    uint32_t nch = (ch == I2S_SLOT_MODE_STEREO) ? 2 : 1;
    /* Ignore invalid sample rates from false MP3 frame headers */
    if (rate < 8000 || rate > 96000) return ESP_OK;
    if (rate == s_last_rate && bits_cfg == s_last_bits && nch == s_last_ch) return ESP_OK;
    s_last_rate = rate; s_last_bits = bits_cfg; s_last_ch = nch;
    i2s_channel_disable(s_i2s_tx_chan);
    i2s_std_clk_config_t clk = { .sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256 };
    i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk);
    i2s_channel_enable(s_i2s_tx_chan);
    return ESP_OK;
}

/* ── Mute ───────────────────────────────────────────────────── */
static esp_err_t mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, (setting == AUDIO_PLAYER_UNMUTE) ? 1 : 0);
    return ESP_OK;
}

/* ── Mixer callback: track end ───────────────────────────────── */
static void mixer_callback(audio_player_cb_ctx_t *ctx)
{
    if (ctx && ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE)
        s_track_done = true;
}

/* ══════════════════════════════════════════════════════════════
 * Radio JSON API
 * ══════════════════════════════════════════════════════════════ */

static esp_err_t radio_fetch(void)
{
    char buf[1024]; int len = 0;
    esp_http_client_config_t cfg = { .url = RADIO_API_URL, .timeout_ms = 5000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return ESP_FAIL; }
    esp_http_client_fetch_headers(c);
    while (len < 1023) { int r = esp_http_client_read(c, buf + len, 1023 - len); if (r <= 0) break; len += r; }
    buf[len] = 0;
    int st = esp_http_client_get_status_code(c);
    esp_http_client_close(c); esp_http_client_cleanup(c);
    if (st != 200 || len == 0) return ESP_FAIL;

    ESP_LOGI(TAG, "Radio JSON: %s", buf);
    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;
    cJSON *jp = cJSON_GetObjectItem(root, "playlist");
    cJSON *jn = cJSON_GetObjectItem(root, "name");
    cJSON *jv = cJSON_GetObjectItem(root, "volume");
    if (jp && cJSON_IsString(jp)) strncpy(s_playlist_url, jp->valuestring, 255);
    if (jn && cJSON_IsString(jn)) strncpy(s_radio_name, jn->valuestring, 63);
    if (jv && cJSON_IsNumber(jv)) {
        double v = cJSON_GetNumberValue(jv);
        s_radio_volume_pct = (v <= 1.0) ? (int)(v * 100 + 0.5) : (int)(v + 0.5);
        if (s_radio_volume_pct < 0) s_radio_volume_pct = 0;
        if (s_radio_volume_pct > 100) s_radio_volume_pct = 100;
        ESP_LOGI(TAG, "Radio: vol=%.1f -> %d%%", v, s_radio_volume_pct);
    }
    cJSON_Delete(root);
    return (s_playlist_url[0]) ? ESP_OK : ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════════
 * M3U Playlist
 * ══════════════════════════════════════════════════════════════ */

static void parse_extinf(const char *line, char *out, size_t sz)
{
    out[0] = 0;
    const char *p = strchr(line, ',');
    if (!p) return;
    p++;
    if ((unsigned char)p[0]==0xF0 && (unsigned char)p[1]==0x9F && (unsigned char)p[2]==0x8E && (unsigned char)p[3]==0xB5) { p+=4; if (*p==' ') p++; }
    strncpy(out, p, sz-1); out[sz-1]=0;
}

static esp_err_t playlist_fetch(void)
{
    if (!s_playlist_url[0]) return ESP_FAIL;
    char *buf = malloc(12288);
    if (!buf) return ESP_ERR_NO_MEM;
    int len = 0;
    esp_http_client_config_t cfg = { .url = s_playlist_url, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); return ESP_FAIL; }
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); free(buf); return ESP_FAIL; }
    esp_http_client_fetch_headers(c);
    while (len < 12287) { int r = esp_http_client_read(c, buf + len, 12287 - len); if (r <= 0) break; len += r; }
    buf[len] = 0;
    esp_http_client_close(c); esp_http_client_cleanup(c);

    if (s_tracks) {
        for (int i = 0; i < s_track_count; i++) { free(s_tracks[i].url); free(s_tracks[i].name); }
        free(s_tracks); s_tracks = NULL;
    }
    s_tracks = calloc(MAX_TRACKS, sizeof(track_t));
    if (!s_tracks) { free(buf); return ESP_ERR_NO_MEM; }
    s_track_count = 0;

    char *saveptr, *line = strtok_r(buf, "\r\n", &saveptr);
    char pname[128] = {0};
    while (line && s_track_count < MAX_TRACKS) {
        if (line[0]==0 || (line[0]=='#' && strncmp(line,"#EXTINF:",8)!=0)) { line=strtok_r(NULL,"\r\n",&saveptr); continue; }
        if (strncmp(line, "#EXTINF:", 8) == 0) parse_extinf(line, pname, 128);
        else if (strncmp(line, "http", 4) == 0) {
            s_tracks[s_track_count].url = strdup(line);
            s_tracks[s_track_count].name = pname[0] ? strdup(pname) : strdup("Unknown");
            s_track_count++;
            pname[0] = 0;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    free(buf);
    ESP_LOGI(TAG, "M3U: %d tracks", s_track_count);
    for (int i = 0; i < (s_track_count < 5 ? s_track_count : 5); i++)
        ESP_LOGI(TAG, "  [%d] %s", i, s_tracks[i].name);
    return s_track_count ? ESP_OK : ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════════
 * Audio init / deinit
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_init(void)
{
    if (s_i2s_ready) return ESP_OK;
    ESP_LOGI(TAG, "Init I2S");
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    i2s_new_channel(&cc, &s_i2s_tx_chan, NULL);
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk=I2S_GPIO_UNUSED, .bclk=CONFIG_PIN_I2S_SCLK, .ws=CONFIG_PIN_I2S_LROUT, .dout=CONFIG_PIN_I2S_SDIN, .din=I2S_GPIO_UNUSED },
    };
    i2s_channel_init_std_mode(s_i2s_tx_chan, &sc);
    i2s_channel_enable(s_i2s_tx_chan);
    s_i2s_ready = true;
    ESP_LOGI(TAG, "I2S OK");
    return ESP_OK;
}

void audio_deinit(void)
{
    audio_stop();
    if (s_mixer_ready) { audio_mixer_deinit(); s_mixer_ready = false; }
    if (s_i2s_tx_chan) { i2s_channel_disable(s_i2s_tx_chan); i2s_del_channel(s_i2s_tx_chan); s_i2s_tx_chan = NULL; }
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);
    s_i2s_ready = false;
}

/* ══════════════════════════════════════════════════════════════
 * Track playback: esp_http_client in main task + feeder task
 * ══════════════════════════════════════════════════════════════ */

static esp_http_client_handle_t s_http_client = NULL;
static RingbufHandle_t s_data_rb = NULL;
static volatile bool s_http_eof = false;
static TaskHandle_t s_feeder_task = NULL;

static void feeder_task(void *arg);

/* Stream I/O: decoder reads from ring buffer */
static size_t rb_read(void *ctx, void *buf, size_t size)
{
    RingbufHandle_t rb = (RingbufHandle_t)ctx;
    size_t got = 0;
    void *item = xRingbufferReceiveUpTo(rb, &got, pdMS_TO_TICKS(200), size);
    if (item) { memcpy(buf, item, got); vRingbufferReturnItem(rb, item); return got; }
    return 0;
}
static int rb_eof(void *ctx) { (void)ctx; return s_http_eof; }
static int rb_seek(void *ctx, long o, int w) { return -1; }
static long rb_tell(void *ctx) { return -1; }
static void rb_close(void *ctx) {}
static const audio_stream_io_ops_t s_rb_ops = {
    .read = rb_read, .seek = rb_seek, .tell = rb_tell, .eof = rb_eof, .close = rb_close
};

static esp_err_t track_open(const char *url)
{
    audio_stop();

    ESP_LOGI(TAG, "Stream: %s", url);

    /* Connect via esp_http_client in main task (works on all APs) */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = false,
    };
    s_http_client = esp_http_client_init(&cfg);
    if (!s_http_client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(s_http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(s_http_client); s_http_client = NULL;
        return err;
    }

    int clen = esp_http_client_fetch_headers(s_http_client);
    ESP_LOGI(TAG, "HTTP connected, len=%d", clen);

    /* Create audio decoder FIRST (largest alloc, needs heap room) */
    audio_stream_config_t scfg = DEFAULT_AUDIO_STREAM_CONFIG("track");
    s_stream = audio_stream_new(&scfg);
    if (!s_stream) {
        ESP_LOGE(TAG, "stream_new failed");
        esp_http_client_close(s_http_client);
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
        return ESP_FAIL;
    }

    /* Ring buffer: HTTP read → decoder */
    s_data_rb = xRingbufferCreate(6 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_data_rb) {
        audio_stream_delete(s_stream); s_stream = NULL;
        esp_http_client_close(s_http_client);
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Init mixer */
    audio_mixer_config_t mixer_cfg = {
        .mute_fn = mute_fn, .clk_set_fn = i2s_reconfig_clk, .write_fn = i2s_write,
        .priority = 5, .coreID = tskNO_AFFINITY,
        .i2s_format = { .sample_rate = 44100, .bits_per_sample = 16, .channels = 2 },
    };
    audio_mixer_init(&mixer_cfg);
    s_mixer_ready = true;
    audio_mixer_callback_register(mixer_callback);

    /* Create stream IO wrapping the ring buffer */
    audio_stream_io_handle_t io = audio_stream_io_create(&s_rb_ops, s_data_rb);
    if (!io) {
        ESP_LOGE(TAG, "io_create failed");
        audio_stop();
        return ESP_FAIL;
    }

    /* Start playback immediately — decoder will wait for data */
    s_track_done = false;
    if (audio_stream_play_io(s_stream, io) != ESP_OK) {
        ESP_LOGE(TAG, "play_io failed");
        audio_stop();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Track playing");

    s_http_eof = false;

    /* Background feeder: esp_http_client_read → ring buffer */
    xTaskCreatePinnedToCore(feeder_task, "audio_feed", 3*1024, NULL, 6, &s_feeder_task, tskNO_AFFINITY);

    return ESP_OK;
}

static void feeder_task(void *arg)
{
    while (!s_http_eof && s_http_client) {
        uint8_t buf[1024];
        int r = esp_http_client_read(s_http_client, (char *)buf, sizeof(buf));
        if (r > 0) {
            while (xRingbufferSend(s_data_rb, buf, r, pdMS_TO_TICKS(500)) != pdTRUE) {
                if (s_http_eof) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else if (r == 0) {
            s_http_eof = true;
        } else {
            s_http_eof = true;
        }
    }
    if (s_http_client) {
        esp_http_client_close(s_http_client);
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
    }
    s_feeder_task = NULL;
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════ */

esp_err_t audio_play_radio(void)
{
    if (!s_i2s_ready) { ESP_LOGE(TAG, "I2S not ready"); return ESP_ERR_INVALID_STATE; }
    if (s_track_count == 0 || s_current_track >= s_track_count) {
        if (radio_fetch() != ESP_OK) return ESP_FAIL;
        if (playlist_fetch() != ESP_OK) return ESP_FAIL;
        s_current_track = 0;
    }
    return track_open(s_tracks[s_current_track].url);
}

esp_err_t audio_play_url(const char *url)
{
    return track_open(url);
}

void audio_service(void)
{
    /* Track ended normally (EOF from mixer callback) */
    if (s_track_done) {
        s_track_done = false;
        audio_stop();
        s_current_track++;
        vTaskDelay(pdMS_TO_TICKS(300));
    } else {
        return;
    }

    if (s_current_track >= s_track_count) {
        ESP_LOGI(TAG, "Playlist done, re-fetch");
        if (radio_fetch() == ESP_OK && playlist_fetch() == ESP_OK) s_current_track = 0;
        else s_current_track = 0;
    }
    ESP_LOGI(TAG, "Next [%d/%d]", s_current_track+1, s_track_count);
    track_open(s_tracks[s_current_track].url);
}

esp_err_t audio_stop(void)
{
    s_http_eof = true;
    if (s_stream) { audio_stream_stop(s_stream); audio_stream_delete(s_stream); s_stream = NULL; }
    for (int i = 0; i < 50; i++) { if (!s_feeder_task) break; vTaskDelay(pdMS_TO_TICKS(10)); }
    if (s_http_client) { esp_http_client_close(s_http_client); esp_http_client_cleanup(s_http_client); s_http_client = NULL; }
    if (s_data_rb) { vRingbufferDelete(s_data_rb); s_data_rb = NULL; }
    return ESP_OK;
}

const char *audio_get_station_name(void)
{
    if (s_current_track >= 0 && s_current_track < s_track_count && s_tracks[s_current_track].name)
        return s_tracks[s_current_track].name;
    if (s_radio_name[0]) return s_radio_name;
    return s_tracks[s_current_track].url;
}

bool audio_is_playing(void)
{
    return s_http_client != NULL || s_stream != NULL;
}
