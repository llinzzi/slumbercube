#pragma once

#include "esp_err.h"
#include "weather_service.h"

void main_screen_init(void);
void main_screen_set_weather_data(const weather_data_t *data);
void main_screen_tick(void);
