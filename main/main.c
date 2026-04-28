#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "lvgl.h"
#include "wifi.h"
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

static void button_long_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Button long pressed, syncing time via WiFi...");
    wifi_init_sta();
    wifi_mark_time_set();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", ACTIVE_DURATION_SECS);

    // Initialize SSD1322 driver first
    ESP_ERROR_CHECK(ssd1322_init());
    ssd1322_display_on();

    // Initialize button for long-press detection
    button_config_t btn_cfg = {
        .long_press_time = LONG_PRESS_TIME_MS,
        .short_press_time = 0,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO_NUM,
        .active_level = 0,
        .enable_power_save = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);
    if (err == ESP_OK) {
        iot_button_register_cb(g_btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, NULL);
        ESP_LOGI(TAG, "Button initialized on GPIO%d, long press=%dms", BUTTON_GPIO_NUM, LONG_PRESS_TIME_MS);
    } else {
        ESP_LOGE(TAG, "Failed to init button on GPIO%d: %s", BUTTON_GPIO_NUM, esp_err_to_name(err));
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

    // Check if time has been set before
    if (!wifi_is_time_set()) {
        ESP_LOGI(TAG, "Time not set, connecting WiFi to sync time");
        ESP_ERROR_CHECK(wifi_init_sta());
        wifi_mark_time_set();
    } else {
        ESP_LOGI(TAG, "Time already set, skipping WiFi");
    }

    // Initialize LVGL adapter
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create UI
    ESP_ERROR_CHECK(ui_wrapper_init());

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
