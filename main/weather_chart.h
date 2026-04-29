#pragma once

#include "lvgl.h"
#include "weather_service.h"

lv_obj_t *weather_chart_create(lv_obj_t *parent);
void weather_chart_set_data(const weather_data_t *data);
void weather_chart_show(void);
void weather_chart_hide(void);
bool weather_chart_is_visible(void);
void weather_chart_set_toggle_cb(void (*cb)(void));
