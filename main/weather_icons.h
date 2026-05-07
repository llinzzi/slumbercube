#pragma once

#include "lvgl.h"

/* Look up icon by weather text. Returns pointer to lv_img_dsc_t. */
const lv_img_dsc_t *weather_icon_match(const char *weather_text);

/* Default icon (clear/sunny) for initial display. */
const lv_img_dsc_t *weather_icon_default(void);
