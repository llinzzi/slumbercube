#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_play_radio(void);
esp_err_t audio_play_url(const char *url);  /* legacy: single URL */
esp_err_t audio_stop(void);
void audio_deinit(void);
const char *audio_get_station_name(void);
bool audio_is_playing(void);
void audio_service(void);  /* call periodically to advance tracks */

#ifdef __cplusplus
}
#endif
