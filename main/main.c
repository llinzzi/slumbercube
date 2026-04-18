#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "lvgl.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL");

    // 通电后等待1秒让硬件稳定
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 初始化SSD1322驱动
    ESP_ERROR_CHECK(ssd1322_init());
    
    // 初始化LVGL适配层
    ESP_ERROR_CHECK(lvgl_adapter_init());
    
    // 等待LVGL任务启动
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 创建UI界面
    ESP_ERROR_CHECK(ui_wrapper_init());
    
    // 等待UI加载
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 强制刷新屏幕
    lv_refr_now(NULL);
    
    ESP_LOGI(TAG, "All initialized successfully");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}