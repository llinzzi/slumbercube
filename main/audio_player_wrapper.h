#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_play_url(const char *url);
esp_err_t audio_stop(void);
void audio_deinit(void);
esp_err_t audio_radio_refresh(void);
void audio_poll_status(void);
bool audio_is_playing(void);
bool audio_stream_ended(void);
const char *audio_get_station_name(void);

#ifdef __cplusplus
}
#endif
