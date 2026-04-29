#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "lvgl.h"
#include "wifi.h"
#include "weather_service.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "MAIN";

#define ACTIVE_DURATION_SECS 60
#define WAKEUP_GPIO_NUM       3
#define BUTTON_GPIO_NUM       3
#define LONG_PRESS_TIME_MS    3000

static button_handle_t g_btn = NULL;
static weather_data_t s_weather;

static void button_short_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Short press, toggling weather/time");
    screens_request_toggle();
}

static void button_long_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Button long pressed, syncing time via WiFi...");
    wifi_init_sta();
    wifi_mark_time_set();

    esp_err_t err = weather_fetch(&s_weather);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "--- Weather updated ---");
        for (int i = 0; i < s_weather.count && i < 6; i++) {
            ESP_LOGI(TAG, "  H%02d: %d°C pop=%d%% %s",
                     s_weather.hourly[i].hour,
                     s_weather.hourly[i].temp,
                     s_weather.hourly[i].rain_prob,
                     s_weather.hourly[i].text);
        }
        ESP_LOGI(TAG, "  (total %d hours)", s_weather.count);
        screens_set_weather_data_ptr(&s_weather);
    } else {
        ESP_LOGE(TAG, "Weather fetch failed after long press");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", ACTIVE_DURATION_SECS);

    // Initialize SSD1322 driver first
    ESP_ERROR_CHECK(ssd1322_init());
    ssd1322_display_on();

    // Initialize button for short press + long-press detection
    button_config_t btn_cfg = {
        .long_press_time = LONG_PRESS_TIME_MS,
        .short_press_time = 200,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO_NUM,
        .active_level = 0,
        .enable_power_save = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);
    if (err == ESP_OK) {
        iot_button_register_cb(g_btn, BUTTON_SINGLE_CLICK, NULL, button_short_press_cb, NULL);
        iot_button_register_cb(g_btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, NULL);
        ESP_LOGI(TAG, "Button initialized on GPIO%d, short=%dms long=%dms",
                 BUTTON_GPIO_NUM, btn_cfg.short_press_time, LONG_PRESS_TIME_MS);
    } else {
        ESP_LOGE(TAG, "Failed to init button on GPIO%d: %s", BUTTON_GPIO_NUM, esp_err_to_name(err));
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

    // Connect WiFi: sync time on first boot, always fetch weather
    if (!wifi_is_time_set()) {
        ESP_LOGI(TAG, "Time not set, connecting WiFi to sync time");
        ESP_ERROR_CHECK(wifi_init_sta());
        wifi_mark_time_set();
    } else {
        ESP_LOGI(TAG, "Time already set, quick WiFi for weather...");
        wifi_init_sta();  // non-fatal if WiFi fails here
    }

    // Fetch weather data after WiFi is connected
    err = weather_fetch(&s_weather);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Weather fetched: %d hours, first: H%02d %d°C pop=%d%%",
                 s_weather.count,
                 s_weather.count > 0 ? s_weather.hourly[0].hour : 0,
                 s_weather.count > 0 ? s_weather.hourly[0].temp : 0,
                 s_weather.count > 0 ? s_weather.hourly[0].rain_prob : 0);
    } else {
        ESP_LOGW(TAG, "Weather fetch failed, will retry on long press");
    }

    // Initialize LVGL adapter
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create UI
    ESP_ERROR_CHECK(ui_wrapper_init());

    // Pass weather data to screens (if available)
    if (s_weather.valid) {
        screens_set_weather_data_ptr(&s_weather);
    }

    // Wait for UI to load
    vTaskDelay(pdMS_TO_TICKS(100));

    // Force screen refresh
    lv_refr_now(NULL);

    ESP_LOGI(TAG, "Running for %d seconds before sleep", ACTIVE_DURATION_SECS);

    // Run for 60 seconds with periodic UI updates
    for (int i = 0; i < ACTIVE_DURATION_SECS; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ui_tick();
    }

    ESP_LOGI(TAG, "Time to sleep, turning off display");

    // Turn off display before deep sleep
    ssd1322_display_off();

    // Configure GPIO3 low-level as wakeup source
    esp_deep_sleep_enable_gpio_wakeup((1ULL << WAKEUP_GPIO_NUM), ESP_GPIO_WAKEUP_GPIO_LOW);

    ESP_LOGI(TAG, "Entering deep sleep, GPIO%d will wake on low level", WAKEUP_GPIO_NUM);

    // Enter deep sleep
    esp_deep_sleep_start();
}
