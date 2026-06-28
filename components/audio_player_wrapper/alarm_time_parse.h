/* Alarm time string parser — host-testable helper.
 *
 * Extracted from audio_player_wrapper.c so it can be unit-tested on the
 * host with no ESP-IDF dependency. The full JSON parse stays in the wrapper
 * (it depends on cJSON + ESP_LOG); this file only validates the "HH:MM"
 * format that comes out of cJSON_GetObjectItem(...).valuestring.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a "HH:MM" 24h time string.
 *  - strict format: exactly 5 chars, two zero-padded digits, ':', two digits
 *  - hour must be 0-23, minute 0-59
 *  - rejects strings with trailing garbage ("07:50:00", "07:50 ", etc.)
 *
 * Returns true on success and writes hour/minute (0-23 / 0-59).
 * Returns false on any other input (NULL, empty, wrong format, out of range).
 */
bool alarm_parse_time_string(const char *s, uint8_t *hour, uint8_t *minute);

#ifdef __cplusplus
}
#endif