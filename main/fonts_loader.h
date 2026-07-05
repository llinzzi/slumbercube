#pragma once
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fonts provided by the fonts flash partition (via fonts_loader_init).
 * These replace font_digital.c / font_station.c in the OTA build.
 * Declared as struct values — usage is &lv_font_digital / &lv_font_station. */
extern lv_font_t lv_font_digital;
extern const lv_font_t lv_font_station;

/* Load fonts from the dedicated fonts flash partition.
 * Must be called after NVS init and before LVGL rendering uses lv_font_*.
 * Returns ESP_OK on success, or an error if the fonts partition is missing,
 * corrupt, or incompatible. */
esp_err_t fonts_loader_init(void);

#ifdef __cplusplus
}
#endif
