#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "lvgl.h"
#include "wifi.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL");

    // Initialize WiFi first (blocks until connected)
    ESP_ERROR_CHECK(wifi_init_sta());

    // Initialize SSD1322 driver
    ESP_ERROR_CHECK(ssd1322_init());

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

    ESP_LOGI(TAG, "All initialized successfully");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
