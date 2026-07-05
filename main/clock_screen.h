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
/* Night mode override: -1 = auto (time-based), 0 = force day, 1 = force night.
 * Resets to auto (-1) on every wake. Valid only for the current session. */
void clock_screen_set_night_override(int8_t override);
int8_t clock_screen_get_night_override(void);
void clock_screen_set_station_name(const char *name);
void clock_screen_set_audio_indicator(bool on);
/* Free the canvas pixel buffer (16KB). Safe to call before deep sleep;
 * canvas will no longer be drawable after this. */
void clock_screen_deinit(void);

/* Show indoor temperature + humidity from SHTC3 sensor.
 * Pass NAN temp for sensor-less variant — label stays blank. */
void clock_screen_set_indoor_env(float temp_c, float humidity);

/* No-network display: show indoor temp + humidity on the right side
 * (replaces weather text). */
void clock_screen_set_indoor_full(float temp_c, float humidity);

/* No-network display: show alarm time below indoor temp. */
void clock_screen_set_alarm_time(int hour, int minute);

/* Show button hint at the bottom (no-network display). */
void clock_screen_show_button_hint(void);
