#ifndef LVGL_ADAPTER_H
#define LVGL_ADAPTER_H

#include "lvgl.h"
#include "esp_err.h"

/**
 * @brief 初始化LVGL适配层
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t lvgl_adapter_init(void);

/* Force a full-screen render from the main task. */
void lvgl_adapter_refr_now(void);

#endif // LVGL_ADAPTER_H
