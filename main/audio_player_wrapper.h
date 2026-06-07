#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_play_url(void);
esp_err_t audio_stop(void);
void audio_deinit(void);
const char *audio_get_station_name(void);

/* True when the current track has finished (decoder idle) or no track is
 * active. Poll this to auto-advance to the next song. */
bool audio_is_finished(void);

/* Returns download progress 0-100, or -1 if unknown/not playing. */
int audio_get_progress(void);

#ifdef __cplusplus
}
#endif
