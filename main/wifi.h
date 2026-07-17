#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

/* Blocking: ensure STA + wait for connection. Returns ESP_OK once the link
 * is up, ESP_FAIL if the link didn't come up within timeout_ms. Idempotent
 * — if the radio is already connected, returns ESP_OK immediately.
 *
 * This is the "I really need the link right now" gate; sibling wifi_sta_ensure()
 * is the non-blocking equivalent. Does NOT touch the SNTP client — call
 * wifi_start_sntp() to start time sync once the link is up. */
esp_err_t wifi_sta_connect(uint32_t timeout_ms);

/* App-layer hook: ensure the SNTP client is running. Idempotent (returns
 * ESP_OK immediately if SNTP was already started). Returns ESP_ERR_INVALID_STATE
 * if wifi hasn't connected yet — sequence wifi_sta_connect() first.
 * This is the only call site application code should use to start time
 * sync. */
esp_err_t wifi_start_sntp(void);

/* Non-blocking: init + start radio, return immediately.
 * Poll wifi_is_connected() to detect when the link comes up. */
esp_err_t wifi_sta_ensure(void);

esp_err_t wifi_ensure_netif(void);
bool wifi_is_connected(void);

bool wifi_is_time_set(void);
void wifi_mark_time_set(void);
void wifi_set_timezone(void);

/* True once SNTP has actually applied a server response to the system clock
 * since the most recent (re-)initialisation. Resets to false on every
 * sntp_init() / wifi_sta_connect() so a caller waiting on this can be sure
 * the value reflects a real round-trip, not a stale carry-over. */
bool wifi_is_time_synced(void);

/* Returns the device's WiFi MAC as hex, e.g. "543204470984".
 * Suitable for /api/esp/<device_id> endpoints. */
const char *wifi_get_device_id(void);

/* Set to true to keep the wifi event handler from auto-connecting to a
 * stale STA while the SoftAP is up (used during provisioning). The flag
 * affects WIFI_EVENT_STA_START and WIFI_EVENT_STA_DISCONNECTED handling. */
void wifi_suppress_auto_connect(bool suppress);

/* Mark the WiFi radio as started (called by start_softap after esp_wifi_start).
 * Keeps wifi.c's internal s_wifi_started flag in sync so a later
 * wifi_sta_connect() call knows to stop+bounce the radio before switching to
 * STA mode. */
void wifi_mark_radio_started(void);

/* NVS-persisted WiFi credentials. Lengths sized for IEEE 802.11 max (32 SSID
 * + 64 PSK) plus a NUL each. */
typedef struct {
    char ssid[33];
    char pass[65];
    bool configured;   /* true once a user has successfully provisioned */
} wifi_creds_t;

/* Read credentials from NVS namespace "wifi_cfg".
 *   - ESP_OK                : out is populated
 *   - ESP_ERR_NOT_FOUND     : no credentials saved (caller should fall back to
 *                             menuconfig, or trigger provisioning)
 *   - other                 : NVS error */
esp_err_t wifi_creds_load(wifi_creds_t *out);

/* Persist credentials. Sets configured=1 and commits.
 * Validates non-empty ssid before writing; returns ESP_ERR_INVALID_ARG otherwise. */
esp_err_t wifi_creds_save(const wifi_creds_t *c);

#endif // WIFI_MANAGER_H