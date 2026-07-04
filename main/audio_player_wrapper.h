#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "weather_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Alarm config parsed from /api/esp "alarm" field (HH:MM string). */
typedef struct {
    bool valid;            /* true after a successful parse of time/weekend flags */
    bool disabled;         /* server explicitly sent enabled=false → fully off */
    uint8_t hour;
    uint8_t minute;
    bool weekend_saturday; /* false → skip alarm on Saturday */
    bool weekend_sunday;   /* false → skip alarm on Sunday */
} audio_alarm_config_t;

esp_err_t audio_init(void);
esp_err_t audio_play_url(void);
esp_err_t audio_stop(void);
void audio_deinit(void);
const char *audio_get_station_name(void);

/* Fetch /api/esp and parse both weather + radio URL from the response.
 * Call this once at boot; audio_play_url() reuses the cached URL. */
esp_err_t audio_fetch_api(void);

/* Cached weather parsed from the latest /api/esp response.
 * Returns a pointer to a static struct (valid= false until first fetch). */
const weather_data_t *audio_get_weather(void);

/* True when the current track has finished (decoder idle) or no track is
 * active. Poll this to auto-advance to the next song. */
bool audio_is_finished(void);

/* Returns download progress 0-100, or -1 if unknown/not playing. */
int audio_get_progress(void);

/* True if a non-empty radio URL is cached (from the last /api/esp fetch).
 * Used to skip audio_play_url() when the server returned no URL. */
bool audio_radio_url_is_set(void);

/* Cache latest indoor temp/RH from on-board SHTC3 sensor.
 * Passed as query params (?t=&h=) on the next /api/esp fetch.
 * Pass NAN for both values on sensor-less hardware — query params omitted. */
void audio_set_indoor_env(float temp_c, float humidity);

/* Set wake source for this cycle ("rtc", "btn", or NULL for cold boot).
 * Must be called once at boot before any /api/esp fetch.
 * The source is appended as ?wake=... on all /api/esp requests. */
void audio_set_wake_source(const char *source);

/* Reload agent config from NVS without going through audio_deinit() +
 * audio_init(). Useful if some flow needs to pick up a new host without
 * tearing down the I2S / mixer state. */
esp_err_t audio_agent_reload(void);

/* Alarm time from the latest /api/esp response. Returns pointer to static
 * struct. valid=false until the first successful fetch. */
const audio_alarm_config_t *audio_get_alarm_config(void);

#ifdef __cplusplus
}
#endif
