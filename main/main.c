#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "wifi.h"
#include "clock_screen.h"
#include "esp_sleep.h"
#include <time.h>
#include <sys/time.h>
#include "iot_button.h"
#include "button_gpio.h"
#include "shtc3.h"
#include "pcf85063.h"

#if CONFIG_AUDIO_ENABLE
#include "audio_player_wrapper.h"
#endif

static const char *TAG = "MAIN";

/* 配置项通过 menuconfig 设置 (参见 Kconfig.projbuild) */

static button_handle_t g_btn = NULL;
static volatile bool s_sleep_pending = false;

static volatile bool s_audio_playing = false;
static volatile bool s_audio_toggle_request = false;

/* Try the on-board SHTC3 sensor. Returns true and fills *temp_c on success.
 * Hardware variant without the sensor just returns false; display omits the value. */
static bool read_indoor_env(float *temp_c, float *humidity)
{
    float t = 0, h = 0;
    if (!shtc3_read(&t, &h)) return false;
    *temp_c = t;
    *humidity = h;
    ESP_LOGI(TAG, "SHTC3: indoor %.1f°C, %.0f%%RH", t, h);
    return true;
}

#if CONFIG_PCF85063_ENABLE
/* Read PCF85063 wall-clock and apply it as UTC system time.
 * Returns true if a plausible time was applied. */
static bool apply_pcf85063_time(void)
{
    if (!pcf85063_is_present()) return false;

    pcf85063_datetime_t dt;
    if (pcf85063_read_datetime(&dt) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 read failed");
        return false;
    }
    /* Reject obviously-uninitialised chips (factory default ~2000 or 0xFF). */
    if (dt.year < 2025 || dt.year > 2099) {
        ESP_LOGW(TAG, "PCF85063 year=%u looks uninitialised, skipping", dt.year);
        return false;
    }

    struct tm tm_utc = {
        .tm_sec   = dt.second,
        .tm_min   = dt.minute,
        .tm_hour  = dt.hour,
        .tm_mday  = dt.day,
        .tm_mon   = (int)dt.month - 1,
        .tm_year  = (int)dt.year - 1900,
        .tm_wday  = dt.weekday,
        .tm_yday  = 0,
        .tm_isdst = 0,
    };
    /* mktime() interprets the struct according to TZ — flip TZ to UTC for the
     * conversion, then restore (wifi_set_timezone() will redo it shortly). */
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm_utc);

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "PCF85063 applied: %04u-%02u-%02u %02u:%02u:%02u UTC (wday=%u)",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.weekday);
    return true;
}

/* Configure the alarm to fire at WAKEUP_HOUR:WAKEUP_MINUTE daily, enable INT
 * output, and configure IO0 with internal pull-up. Returns true on success. */
static bool arm_pcf85063_alarm_wakeup(void)
{
    if (!pcf85063_is_present()) return false;

    pcf85063_alarm_t alarm = {
        .enable  = true,
        .minute  = CONFIG_WAKEUP_MINUTE,
        .hour    = CONFIG_WAKEUP_HOUR,
        .day     = PCF85063_ALARM_DISABLE,
        .weekday = PCF85063_ALARM_DISABLE,
    };
    if (pcf85063_set_alarm(&alarm) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 set_alarm failed");
        return false;
    }
    if (pcf85063_enable_alarm_int(true) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 enable_alarm_int failed");
        return false;
    }

    /* PCF85063 INT# is open-drain — internal pull-up keeps the line high
     * until the chip drives it low. */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_PCF85063_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    ESP_LOGI(TAG, "PCF85063 alarm armed for %02d:%02d daily on IO%d",
             CONFIG_WAKEUP_HOUR, CONFIG_WAKEUP_MINUTE, CONFIG_PCF85063_INT_GPIO);
    return true;
}
#endif /* CONFIG_PCF85063_ENABLE */

static void apply_weather_and_indoor(const weather_data_t *w)
{
    if (w && w->valid) {
        screens_set_weather_data_ptr(w);
    }
    float t = 0, h = 0;
    if (read_indoor_env(&t, &h)) {
        clock_screen_set_indoor_env(t, h);
        audio_set_indoor_env(t, h);
    }
}

/* Short click: sleep */
static void button_short_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Short click, going to sleep");
    s_sleep_pending = true;
}

/* Long press: request audio toggle. Main loop handles WiFi + playback. */
static void button_long_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Long press");
#if CONFIG_AUDIO_ENABLE
    s_audio_toggle_request = true;
    /* Show indicator immediately when starting, hide when stopping */
    clock_screen_set_audio_indicator(!s_audio_playing);
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring long press");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", CONFIG_ACTIVE_DURATION_SECS);

    /* Detect wake source for /api/esp ?wake= query param */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        audio_set_wake_source("rtc");
        ESP_LOGI(TAG, "Woke from RTC timer");
        break;
    case ESP_SLEEP_WAKEUP_GPIO: {
        uint64_t wake_pins = esp_sleep_get_gpio_wakeup_status();
#if CONFIG_PCF85063_ENABLE
        if (wake_pins & (1ULL << CONFIG_PCF85063_INT_GPIO)) {
            audio_set_wake_source("rtc");
            ESP_LOGI(TAG, "Woke from PCF85063 alarm (IO%d)", CONFIG_PCF85063_INT_GPIO);
            pcf85063_clear_alarm_flag();
            break;
        }
#endif
        audio_set_wake_source("btn");
        ESP_LOGI(TAG, "Woke from GPIO (button IO%d, status=0x%llX)",
                 CONFIG_WAKEUP_GPIO, (unsigned long long)wake_pins);
        break;
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        audio_set_wake_source("sys");
        ESP_LOGI(TAG, "Cold boot");
        break;
    default:
        audio_set_wake_source("sys");
        ESP_LOGI(TAG, "Wake cause: %d (unhandled, using sys)", (int)cause);
        break;
    }

    /* Enable GPIO hold through deep sleep, and release any hold left from
     * previous sleep cycle before reconfiguring pins. */
    gpio_deep_sleep_hold_en();
    gpio_hold_dis(PIN_NUM_RST);
    gpio_hold_dis(CONFIG_PIN_NS4168_CTRL);
    gpio_hold_dis(CONFIG_WAKEUP_GPIO);
#if CONFIG_PCF85063_ENABLE
    gpio_hold_dis(CONFIG_PCF85063_INT_GPIO);
#endif

    /* Hold all control and SPI pins at known levels before SSD1322 init.
     * CS is hardwired to GND, so the SSD1322 SPI is always selected — any
     * floating or transitioning MOSI/SCLK during bootloader can be interpreted
     * as random commands and cause the display to light up with garbage. */
    gpio_config_t early_pins = {
        .pin_bit_mask = (1ULL << PIN_NUM_RST) | (1ULL << PIN_NUM_DC) |
                        (1ULL << PIN_NUM_MOSI) | (1ULL << PIN_NUM_CLK) |
                        (1ULL << CONFIG_PIN_NS4168_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* RST pull-down ensures SSD1322 stays in reset if GPIO floats during bootloader */
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&early_pins);
    gpio_set_level(PIN_NUM_RST, 0);        /* Keep SSD1322 in reset */
    gpio_set_level(PIN_NUM_MOSI, 0);       /* MOSI low */
    gpio_set_level(PIN_NUM_CLK, 0);        /* SCLK low */
    gpio_set_level(PIN_NUM_DC, 0);         /* DC low */
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0); /* NS4168 shutdown */

    // Initialize SSD1322 driver first (display stays OFF until first frame rendered)
    ESP_ERROR_CHECK(ssd1322_init());

    // Initialize button with short/long press distinction
    // - short_press_time 300ms: 容忍机械抖动，普通点击就能识别
    // - long_press_time  800ms: 更短的长按识别，用户体验更灵敏
    button_config_t btn_cfg = {
        .short_press_time = 300,
        .long_press_time = 800,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_BUTTON_GPIO,
        .active_level = 0,
        .enable_power_save = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);
    if (err == ESP_OK) {
        iot_button_register_cb(g_btn, BUTTON_SINGLE_CLICK, NULL, button_short_click_cb, NULL);
        iot_button_register_cb(g_btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, NULL);
        ESP_LOGI(TAG, "Button initialized on GPIO%d (short=toggle audio, long=sleep)", CONFIG_BUTTON_GPIO);
    } else {
        ESP_LOGE(TAG, "Failed to init button on GPIO%d: %s", CONFIG_BUTTON_GPIO, esp_err_to_name(err));
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

#if CONFIG_PCF85063_ENABLE
    /* Init the external RTC and apply its time BEFORE WiFi/SNTP.
     * wifi_set_timezone() above set TZ to local; apply_pcf85063_time() flips
     * TZ to UTC internally for the conversion, then restores via the next
     * wifi_set_timezone() call below. */
    if (pcf85063_init() == ESP_OK) {
        apply_pcf85063_time();
    } else {
        ESP_LOGW(TAG, "PCF85063 not present, time will come from SNTP only");
    }
#endif

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create UI (first frame rendered and flushed to GDDRAM inside this call)
    ESP_ERROR_CHECK(ui_wrapper_init());

    // Turn on display AFTER first frame is in GDDRAM — eliminates white flash on wake
    ssd1322_display_on();

    // Wait for UI to load
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!clock_screen_is_night_time()) {
        // Always init TCP/IP stack + start WiFi (needed for /api/esp)
        clock_screen_set_station_name("Connecting WiFi...");
        wifi_ensure_netif();
        if (wifi_init_sta() == ESP_OK) {
            if (!wifi_is_time_set()) {
                wifi_mark_time_set();
            }
#if CONFIG_PCF85063_ENABLE
            /* wifi_init_sta() blocks for up to 10s on SNTP. If it succeeded,
             * the system clock now has an accurate UTC reading — push it
             * back to PCF85063 so the chip holds the authoritative time and
             * future boots don't depend on WiFi being available. */
            if (pcf85063_is_present()) {
                pcf85063_sync_from_system();
            }
#endif
        }
        /* Weather is fetched as part of audio_play_url() via /api/esp;
         * the display shows the default icon until that completes. */
    } else {
        ESP_LOGI(TAG, "Night mode, skipping network and weather fetch");
    }

#if CONFIG_AUDIO_ENABLE
    /* Start audio playback (non-blocking: mixer + decoder + HTTP tasks run in background).
     * Skip in night mode since WiFi is not available.
     * Read SHTC3 early so indoor temp appears in the very first /api/esp URL,
     * avoiding a second HTTP round-trip with different query params. */
    if (!clock_screen_is_night_time()) {
        /* Read indoor sensor before any network call */
        float t = 0, h = 0;
        if (shtc3_read(&t, &h)) {
            audio_set_indoor_env(t, h);
            ESP_LOGI(TAG, "SHTC3: indoor %.1f°C, %.0f%%RH", t, h);
        }

        if (audio_init() == ESP_OK) {
            /* 1. Single HTTP call — parses both weather and radio URL from /api/esp.
             * Weather and URL are independent; one can succeed without the other. */
            clock_screen_set_station_name("Fetching weather...");
            bool got_weather = (audio_fetch_api() == ESP_OK);

            /* Display whatever we got (best-effort) */
            const weather_data_t *w = audio_get_weather();
            if (got_weather && w && w->valid) {
                screens_set_weather_data_ptr(w);
            }
            clock_screen_set_indoor_env(t, h);

            if (got_weather) {
                clock_screen_set_station_name("Starting audio...");
            }
            /* Give LVGL one tick to render before audio starts buffering */
            vTaskDelay(pdMS_TO_TICKS(50));

            /* 2. Start audio — only if the same API response included a URL.
             * audio_fetch_api() already cached s_radio_url above. */
            if (audio_radio_url_is_set()) {
                if (audio_play_url() == ESP_OK) {
                    clock_screen_set_station_name(audio_get_station_name());
                    clock_screen_set_audio_indicator(true);
                    s_audio_playing = true;
                }
            }
        }
    }
#endif

    ESP_LOGI(TAG, "Running for %d seconds before sleep, button wakes", CONFIG_ACTIVE_DURATION_SECS);

    // Main loop: button press or timeout → sleep
    for (int i = 0; i < CONFIG_ACTIVE_DURATION_SECS; i++) {
        if (s_sleep_pending) {
            break;
        }

#if CONFIG_AUDIO_ENABLE
        /* Handle long-press audio toggle request */
        if (s_audio_toggle_request) {
            s_audio_toggle_request = false;
            if (s_audio_playing) {
                audio_stop();
                clock_screen_set_audio_indicator(false);
                clock_screen_set_station_name("Paused");
                s_audio_playing = false;
            } else {
                clock_screen_set_station_name("Connecting WiFi...");
                wifi_ensure_netif();
                if (wifi_init_sta() == ESP_OK) {
                    clock_screen_set_station_name("Starting audio...");
                    if (audio_init() == ESP_OK) {
                        if (audio_play_url() == ESP_OK) {
                            clock_screen_set_station_name(audio_get_station_name());
                            apply_weather_and_indoor(audio_get_weather());
                            clock_screen_set_audio_indicator(true);
                            s_audio_playing = true;
                        }
                    }
                } else {
                    /* wifi_init_sta timed out — check if it connected just after timeout */
                    if (wifi_is_connected()) {
                        clock_screen_set_station_name("Starting audio...");
                        if (audio_init() == ESP_OK) {
                            if (audio_play_url() == ESP_OK) {
                                clock_screen_set_station_name(audio_get_station_name());
                                apply_weather_and_indoor(audio_get_weather());
                                clock_screen_set_audio_indicator(true);
                                s_audio_playing = true;
                            }
                        }
                    } else {
                        clock_screen_set_audio_indicator(false);
                        clock_screen_set_station_name("WiFi failed");
                    }
                }
            }
        }

        /* Auto-advance: when a track finishes, fetch the next song from /api/esp
         * and continue playing in a loop.
         *
         * Two conditions trigger advance:
         * 1. Stream state goes IDLE (clean finish)
         * 2. HTTP download at 100% for 3+ seconds (decoder stuck on trailing garbage) */
        if (s_audio_playing && !s_audio_toggle_request) {
            int progress = audio_get_progress();
            static int stall_ticks = 0;
            bool track_done = false;

            if (audio_is_finished()) {
                const char *name = audio_get_station_name();
                ESP_LOGI(TAG, "Track finished cleanly: '%s', advancing to next", name ? name : "unknown");
                track_done = true;
            } else if (progress >= 100) {
                stall_ticks++;
                ESP_LOGW(TAG, "Download complete but decoder stalled for %d s", (int)stall_ticks);
                if (stall_ticks >= 3) {
                    ESP_LOGW(TAG, "Decoder stalled, force-advancing to next track");
                    track_done = true;
                }
            } else {
                stall_ticks = 0;
            }

            if (track_done) {
                stall_ticks = 0;
                audio_deinit();
                ESP_LOGI(TAG, "Audio deinitialized, fetching next song...");
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* Re-init I2S (deinit tore it down), then fetch next song and play */
                clock_screen_set_station_name("Next song...");
                if (!wifi_is_connected()) {
                    wifi_init_sta();
                }
                if (audio_init() == ESP_OK) {
                    if (audio_play_url() == ESP_OK) {
                        clock_screen_set_station_name(audio_get_station_name());
                        apply_weather_and_indoor(audio_get_weather());
                        clock_screen_set_audio_indicator(true);
                    } else {
                        ESP_LOGW(TAG, "Failed to fetch next track, stopping");
                        clock_screen_set_audio_indicator(false);
                        clock_screen_set_station_name("Paused");
                        s_audio_playing = false;
                    }
                } else {
                    ESP_LOGW(TAG, "Audio re-init failed, stopping");
                    clock_screen_set_audio_indicator(false);
                    clock_screen_set_station_name("Paused");
                    s_audio_playing = false;
                }
            }
        }

#endif

        /* Refresh indoor temp every 10s — updates display + cache for next /api/esp */
        if (i % 10 == 0) {
            float t = 0, h = 0;
            if (shtc3_read(&t, &h)) {
                audio_set_indoor_env(t, h);
                clock_screen_set_indoor_env(t, h);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Time to sleep, turning off display");

    /* Kill display and amp instantly so user sees/hears immediate shutdown.
     * Audio cleanup (stopping the HTTP download task) takes a few seconds
     * and must happen after these to avoid visible delay. */
    ssd1322_display_off();
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);

#if CONFIG_AUDIO_ENABLE
    audio_stop();
    audio_deinit();
#endif

    // Drive RST low and hold through deep sleep to prevent SSD1322 from
    // exiting reset during wake transition (which causes white flash)
    gpio_set_level(PIN_NUM_RST, 0);
    gpio_hold_en(PIN_NUM_RST);

    // Hold NS4168 CTRL low through deep sleep to keep audio amp off
    gpio_hold_en(CONFIG_PIN_NS4168_CTRL);

    /* Enable internal pull-up on wakeup GPIO for reliable deep-sleep wake */
    gpio_set_pull_mode(CONFIG_WAKEUP_GPIO, GPIO_PULLUP_ONLY);
    gpio_hold_en(CONFIG_WAKEUP_GPIO);

#if CONFIG_PCF85063_ENABLE
    /* Arm the PCF85063 alarm before sleep. If successful, we'll wake from
     * IO0 (PCF85063 INT#) instead of an internal RTC timer — the external
     * crystal keeps ticking accurately while ESP32-C3 is fully powered down. */
    bool use_pcf85063_wakeup = arm_pcf85063_alarm_wakeup();
#else
    bool use_pcf85063_wakeup = false;
#endif

    // Configure wakeup sources: button always; PCF85063 INT if available.
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    uint64_t wake_mask = (1ULL << CONFIG_WAKEUP_GPIO);
#if CONFIG_PCF85063_ENABLE
    if (use_pcf85063_wakeup) {
        wake_mask |= (1ULL << CONFIG_PCF85063_INT_GPIO);
        gpio_hold_en(CONFIG_PCF85063_INT_GPIO);
    }
#endif
    esp_deep_sleep_enable_gpio_wakeup(wake_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    ESP_LOGI(TAG, "Entering deep sleep, GPIO wakeup mask 0x%llX",
             (unsigned long long)wake_mask);
#else
    ESP_LOGW(TAG, "GPIO deep sleep wakeup not supported on this chip, wake by timer only");
#endif

    // Fallback: schedule internal RTC timer wakeup if PCF85063 alarm isn't armed.
    if (!use_pcf85063_wakeup) {
        time_t now = time(NULL);
        struct tm tm_now = {0};
        localtime_r(&now, &tm_now);

        struct tm tm_wake = tm_now;
        tm_wake.tm_hour = CONFIG_WAKEUP_HOUR;
        tm_wake.tm_min = CONFIG_WAKEUP_MINUTE;
        tm_wake.tm_sec = 0;
        time_t wake_time = mktime(&tm_wake);
        if (wake_time <= now) {
            wake_time += 24 * 60 * 60;  // next day
        }
        uint64_t sleep_us = (uint64_t)(wake_time - now) * 1000000ULL;
        esp_sleep_enable_timer_wakeup(sleep_us);
        ESP_LOGI(TAG, "Timer wakeup in %llu min (%02d:%02d)",
                 (unsigned long long)(sleep_us / 60000000),
                 CONFIG_WAKEUP_HOUR, CONFIG_WAKEUP_MINUTE);
    }

    // Enter deep sleep
    esp_deep_sleep_start();
}
