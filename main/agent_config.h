#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* SlumberCube Agent server configuration.
 *
 * Stored in NVS namespace "agent_cfg" (separate from "wifi_cfg" and "clock"
 * so the lifecycle of agent settings is independent of WiFi creds). When
 * `enabled` is false, audio_player_wrapper.c short-circuits the /api/esp
 * HTTP fetch and the device runs as a clock + indoor-temp display only.
 *
 * The user-configurable field is the host only — the port (:3000) and path
 * (/api/esp) are part of the SlumberCube Agent backend contract and stay
 * hardcoded in audio_player_wrapper.c.
 */
#define AGENT_HOST_MAX 64   /* NVS string max is 64 bytes incl. NUL → usable 63 */

typedef struct {
    char host[AGENT_HOST_MAX + 1];  /* NUL-terminated bare host (no scheme/port/path) */
    bool enabled;                    /* false → skip /api/esp entirely */
} agent_config_t;

/* Read agent config from NVS. If no entry exists (first boot, or
 * nvs_flash_init/NVS failure), populates defaults: host="192.168.8.192",
 * enabled=false, and returns ESP_OK. Returns ESP_OK on a successful read
 * (possibly with defaults filled in), or a non-OK esp_err_t only if NVS
 * itself is broken. */
esp_err_t agent_config_load(agent_config_t *out);

/* Persist agent config to NVS. Does NOT call nvs_flash_init() (caller must
 * have initialised NVS already — same convention as wifi_creds_save).
 * Validates non-empty host before writing. */
esp_err_t agent_config_save(const agent_config_t *c);
