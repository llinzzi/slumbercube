#pragma once

#include <stdint.h>

#define LOADING_IMG_W 48
#define LOADING_IMG_H 52

/* Returns pointer to L8 pixel data. */
const uint8_t *loading_img_get(void);
