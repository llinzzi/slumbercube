#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#define WEATHER_MAX_DAYS 1  /* /api/esp returns single-day conditions */

typedef struct {
    int high;               /* tempMax — today's day high */
    int low;                /* tempMin — today's night low */
    int current;            /* temp    — current observed */
    int humidity;           /* humidity % */
    char day_text[16];      /* textDay   — e.g. "多云" */
    char night_text[16];    /* textNight */
    char current_text[16];  /* text      — current observation */
    int month;              /* 1-12 */
    int day;                /* 1-31 */
} daily_forecast_t;

typedef struct {
    daily_forecast_t daily[WEATHER_MAX_DAYS];
    int count;
    bool valid;
    time_t fetch_time;
} weather_data_t;

/* Legacy entry point — AMAP path removed.
 * Returns ESP_ERR_NOT_SUPPORTED; weather now comes from /api/esp via
 * audio_player_wrapper.c::audio_get_weather(). */
esp_err_t weather_fetch(weather_data_t *data);
