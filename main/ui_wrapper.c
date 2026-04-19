#include "ui.h"
#include "lvgl_adapter.h"
#include "esp_log.h"

static const char *TAG = "UI_WRAPPER";

esp_err_t ui_wrapper_init(void)
{
    ui_init();

    lvgl_adapter_set_ui_ready();

    ESP_LOGI(TAG, "LVGL UI initialized successfully");
    return ESP_OK;
}
