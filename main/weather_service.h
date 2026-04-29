#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#define WEATHER_MAX_HOURLY 24

typedef struct {
    int hour;
    int temp;
    int rain_prob;
    float rain_mm;
    char icon[8];
    char text[32];
} hourly_forecast_t;

typedef struct {
    hourly_forecast_t hourly[WEATHER_MAX_HOURLY];
    int count;
    bool valid;
    time_t fetch_time;
} weather_data_t;

esp_err_t weather_fetch(weather_data_t *data);
