#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#define WEATHER_MAX_DAYS 4

typedef struct {
    int high;           /* day temperature (daytemp) */
    int low;            /* night temperature (nighttemp) */
    char day_text[16];  /* day weather text, e.g. "小雨" */
    char night_text[16];
    int month;          /* 1-12 */
    int day;            /* 1-31 */
} daily_forecast_t;

typedef struct {
    daily_forecast_t daily[WEATHER_MAX_DAYS];
    int count;
    bool valid;
    time_t fetch_time;
} weather_data_t;

esp_err_t weather_fetch(weather_data_t *data);
