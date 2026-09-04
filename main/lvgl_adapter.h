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
 * @brief Start the LVGL refresh worker after the initial screen is built.
 */
esp_err_t lvgl_adapter_start_task(void);

/**
 * @brief Serialize access to LVGL from application tasks.
 *
 * The lock is recursive because public UI helpers call each other and the
 * LVGL worker invokes the same helpers from ui_tick().
 */
void lvgl_adapter_lock(void);
void lvgl_adapter_unlock(void);

/**
 * @brief Stop the refresh worker before releasing UI-owned buffers.
 */
void lvgl_adapter_stop_task(void);

/* Force a full-screen render from the main task. */
void lvgl_adapter_refr_now(void);

#endif // LVGL_ADAPTER_H
