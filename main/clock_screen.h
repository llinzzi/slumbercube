#pragma once

#include "lvgl.h"
#include "weather_service.h"

lv_obj_t *clock_screen_create(lv_obj_t *parent);
void clock_screen_set_data(const weather_data_t *data);
void clock_screen_update_time(void);
void clock_screen_show(void);
void clock_screen_hide(void);
bool clock_screen_is_visible(void);
bool clock_screen_is_night_time(void);
void clock_screen_set_night_mode(bool enable);
void clock_screen_set_station_name(const char *name);
