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

static const char *TAG = "MAIN";

#define ACTIVE_DURATION_SECS 60
#define WAKEUP_GPIO_NUM       3

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", ACTIVE_DURATION_SECS);

    // Initialize WiFi first (blocks until connected)
    ESP_ERROR_CHECK(wifi_init_sta());

    // Initialize SSD1322 driver
    ESP_ERROR_CHECK(ssd1322_init());

    // Ensure display is on
    ssd1322_display_on();

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
