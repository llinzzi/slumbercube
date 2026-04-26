#ifndef LVGL_ADAPTER_H
#define LVGL_ADAPTER_H

#include "lvgl.h"
#include "esp_err.h"

/**
 * @brief 初始化LVGL适配层
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t lvgl_adapter_init(void);

/**
 * @brief 获取LVGL显示器对象
 * @return LVGL显示器对象指针
 */
lv_display_t* lvgl_adapter_get_display(void);

#endif // LVGL_ADAPTER_H
