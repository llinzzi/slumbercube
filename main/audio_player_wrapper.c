#include "audio_player_wrapper.h"
#include "audio_mixer.h"
#include "audio_stream.h"
#include "audio_http_stream.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static audio_stream_handle_t s_stream = NULL;
static audio_http_stream_handle_t s_http_stream = NULL;
static bool s_i2s_ready = false;
static bool s_mixer_ready = false;
static const char *s_status = NULL;

/* ── Software volume scale (16-bit stereo PCM) ──────────────── */
static void apply_volume(void *buf, size_t len)
{
    int vol = CONFIG_AUDIO_VOLUME_PCT;
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
 * Public API
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
    http_cfg.buffer_size      = 8 * 1024;
    http_cfg.high_watermark   = 6 * 1024;
    http_cfg.low_watermark    = 2 * 1024;
    http_cfg.task_stack_size  = 3 * 1024;
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

    /* Wait for initial buffer — timeout means connection failed */
    for (int wait = 0; wait < 50; wait++) {
        size_t buffered = audio_http_stream_get_buffered_bytes(s_http_stream);
        if (buffered >= 4096) {
            err = audio_stream_play_io(s_stream, io);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Playback started OK");
            }
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timeout waiting for buffer, closing");
    audio_stop();
    s_status = "Network error";
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_play_url(const char *url)
{
    if (!s_i2s_ready) {
        ESP_LOGE(TAG, "I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_status = "Connecting...";

    /* Try primary URL, fall back to secondary */
    if (audio_play_url_inner(url) == ESP_OK) {
        s_status = "Streaming...";
        return ESP_OK;
    }

    s_status = "Switching...";
    ESP_LOGW(TAG, "Primary failed, trying fallback...");
    if (audio_play_url_inner("https://strm112.1.fm/80s_90s_mobile_mp3") == ESP_OK) {
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
    if (s_http_stream) {
        const char *t = audio_http_stream_get_stream_title(s_http_stream);
        if (t) return t;
        const char *n = audio_http_stream_get_icy_name(s_http_stream);
        if (n) return n;
    }
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
