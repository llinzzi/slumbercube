#pragma once

#include "lvgl.h"
#include "audio_player_wrapper.h"

lv_obj_t *clock_screen_create(lv_obj_t *parent);
void clock_screen_set_data(const weather_data_t *data);
void clock_screen_update_time(void);
void clock_screen_show(void);
void clock_screen_hide(void);
bool clock_screen_is_visible(void);
bool clock_screen_is_night_time(void);
void clock_screen_set_night_mode(bool enable);
void clock_screen_set_station_name(const char *name);
void clock_screen_set_audio_indicator(bool on);
/* Free the canvas pixel buffer (16KB). Safe to call before deep sleep;
 * canvas will no longer be drawable after this. */
void clock_screen_deinit(void);

/* Show indoor temperature + humidity from SHTC3 sensor.
 * Pass NAN temp for sensor-less variant — label stays blank. */
void clock_screen_set_indoor_env(float temp_c, float humidity);
