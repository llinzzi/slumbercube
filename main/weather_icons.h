#pragma once

#include <stdint.h>

#define WEATHER_ICON_W 32
#define WEATHER_ICON_H 32

/* Look up icon by weather text. Returns pointer to L8 pixel data (1024 bytes). */
const uint8_t *weather_icon_match(const char *weather_text);

/* Default icon (clear/sunny) for initial display. */
const uint8_t *weather_icon_default(void);
