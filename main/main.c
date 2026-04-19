#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "lvgl.h"
#include "wifi.h"
#include "sleep.h"
#include "esp_sleep.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    bool from_deep_sleep = (wakeup_cause != ESP_SLEEP_WAKEUP_UNDEFINED);

    ESP_LOGI(TAG, "Wakeup cause: %d, from_deep_sleep: %d", wakeup_cause, from_deep_sleep);

    if (!from_deep_sleep) {
        // Cold boot: init WiFi and sync time
        ESP_LOGI(TAG, "Cold boot, initializing WiFi and NTP...");
        wifi_init_sta();
    } else {
        ESP_LOGI(TAG, "Wake from deep sleep, skipping WiFi");
    }

    ESP_ERROR_CHECK(ssd1322_init());
    ESP_ERROR_CHECK(lvgl_adapter_init());

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(ui_wrapper_init());

    vTaskDelay(pdMS_TO_TICKS(100));

    lv_refr_now(NULL);

    // Init sleep manager (starts sleep timer on cold boot)
    sleep_init(from_deep_sleep);

    ESP_LOGI(TAG, "All initialized successfully");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
