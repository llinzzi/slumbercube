#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Download and apply an OTA firmware update from the given URL.
 *
 * The firmware binary is streamed into the inactive OTA partition using
 * esp_http_client + esp_ota_ops.  Progress is shown on the OLED display.
 *
 * On success the device reboots automatically — this function never
 * returns ESP_OK to the caller.
 *
 * On failure the OTA slot is cleaned up and the caller proceeds with
 * normal operation (audio playback, etc.).
 *
 * url — full HTTP URL to the firmware binary (e.g. "http://192.168.1.1:3000/firmware/slumbercube.bin")
 */
esp_err_t ota_perform(const char *url);

#ifdef __cplusplus
}
#endif
