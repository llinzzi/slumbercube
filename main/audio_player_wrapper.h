#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_play_url(const char *url);
esp_err_t audio_stop(void);
void audio_deinit(void);
void audio_get_station_text(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
