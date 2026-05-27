#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "wifi.h"
#include "weather_service.h"
#include "clock_screen.h"
#include "esp_sleep.h"
#include <time.h>
#include "iot_button.h"
#include "button_gpio.h"

#if CONFIG_AUDIO_ENABLE
#include "audio_player_wrapper.h"
#endif

static const char *TAG = "MAIN";

/* 配置项通过 menuconfig 设置 (参见 Kconfig.projbuild) */

static button_handle_t g_btn = NULL;
static weather_data_t s_weather;
static volatile bool s_sleep_pending = false;

static volatile bool s_audio_playing = false;
static volatile bool s_audio_toggle_request = false;

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

    /* Enable GPIO hold through deep sleep, and release any hold left from
     * previous sleep cycle before reconfiguring pins. */
    gpio_deep_sleep_hold_en();
    gpio_hold_dis(PIN_NUM_RST);
    gpio_hold_dis(CONFIG_PIN_NS4168_CTRL);
    gpio_hold_dis(CONFIG_WAKEUP_GPIO);

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
    button_config_t btn_cfg = {
        .short_press_time = 200,
        .long_press_time = 1500,
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

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create UI (first frame rendered and flushed to GDDRAM inside this call)
    ESP_ERROR_CHECK(ui_wrapper_init());

    // Turn on display AFTER first frame is in GDDRAM — eliminates white flash on wake
    ssd1322_display_on();

    // Pass weather data to screens if available (from a previous fetch)
    if (s_weather.valid) {
        screens_set_weather_data_ptr(&s_weather);
    }

    // Wait for UI to load
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!clock_screen_is_night_time()) {
        // Always init TCP/IP stack + start WiFi for button-press weather
        clock_screen_set_station_name("Connecting WiFi...");
        wifi_ensure_netif();
        if (wifi_init_sta() == ESP_OK) {
            if (!wifi_is_time_set()) {
                wifi_mark_time_set();
            }
        }

        // Initial weather fetch (retries handle async WiFi connection)
        clock_screen_set_station_name("Fetching weather...");
        for (int retry = 0; retry < 5; retry++) {
            esp_err_t err = weather_fetch(&s_weather);
            if (err == ESP_OK) {
                screens_set_weather_data_ptr(&s_weather);
                break;
            }
            ESP_LOGW(TAG, "Boot weather fetch attempt %d/5 failed", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    } else {
        ESP_LOGI(TAG, "Night mode, skipping network and weather fetch");
    }

#if CONFIG_AUDIO_ENABLE
    /* Start audio playback (non-blocking: mixer + decoder + HTTP tasks run in background).
     * Skip in night mode since WiFi is not available. */
    if (!clock_screen_is_night_time()) {
        if (audio_init() == ESP_OK) {
            audio_play_url(CONFIG_AUDIO_MUSIC_URL);
            s_audio_playing = true;
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
                        audio_play_url(CONFIG_AUDIO_MUSIC_URL);
                        clock_screen_set_audio_indicator(true);
                        s_audio_playing = true;
                    }
                } else {
                    /* wifi_init_sta timed out — check if it connected just after timeout */
                    if (wifi_is_connected()) {
                        clock_screen_set_station_name("Starting audio...");
                        if (audio_init() == ESP_OK) {
                            audio_play_url(CONFIG_AUDIO_MUSIC_URL);
                            clock_screen_set_audio_indicator(true);
                            s_audio_playing = true;
                        }
                    } else {
                        clock_screen_set_audio_indicator(false);
                        clock_screen_set_station_name("WiFi failed");
                    }
                }
            }
        }

        /* Poll station name from audio stream */
        if (i < 10 || i % 5 == 0) {
            const char *info = audio_get_station_name();
            if (info) {
                clock_screen_set_station_name(info);
            }
        }
#endif

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

    // Configure GPIO3 low-level as wakeup source
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    esp_deep_sleep_enable_gpio_wakeup((1ULL << CONFIG_WAKEUP_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW);
    ESP_LOGI(TAG, "Entering deep sleep, GPIO%d will wake on low level", CONFIG_WAKEUP_GPIO);
#else
    ESP_LOGW(TAG, "GPIO deep sleep wakeup not supported on this chip, wake by timer only");
#endif

    // Schedule timer wakeup at configured time (default 7:50)
    {
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
