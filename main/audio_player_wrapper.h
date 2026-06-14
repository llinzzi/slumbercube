#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "weather_service.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_play_url(void);
esp_err_t audio_stop(void);
void audio_deinit(void);
const char *audio_get_station_name(void);

/* Fetch /api/esp and parse weather only (no audio playback).
 * Call this before audio_play_url() to render weather immediately,
 * then call audio_play_url() to start audio. */
esp_err_t audio_fetch_weather(void);

/* Cached weather parsed from the latest /api/esp response.
 * Returns a pointer to a static struct (valid= false until first fetch). */
const weather_data_t *audio_get_weather(void);

/* True when the current track has finished (decoder idle) or no track is
 * active. Poll this to auto-advance to the next song. */
bool audio_is_finished(void);

/* Returns download progress 0-100, or -1 if unknown/not playing. */
int audio_get_progress(void);

#ifdef __cplusplus
}
#endif
