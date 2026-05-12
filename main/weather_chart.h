#pragma once

#include "lvgl.h"
#include "weather_service.h"

lv_obj_t *weather_chart_create(lv_obj_t *parent);
void weather_chart_set_data(const weather_data_t *data);
void weather_chart_update_time(void);
void weather_chart_show(void);
void weather_chart_hide(void);
bool weather_chart_is_visible(void);
bool weather_chart_is_night_time(void);
void weather_chart_set_night_mode(bool enable);
