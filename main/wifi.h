#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

/* Blocking: init + wait for connection + SNTP (up to 30s). */
esp_err_t wifi_init_sta(void);

/* Non-blocking: init + start radio, return immediately.
 * Poll wifi_is_connected() to detect when the link comes up. */
esp_err_t wifi_sta_ensure(void);

esp_err_t wifi_ensure_netif(void);
bool wifi_is_connected(void);

bool wifi_is_time_set(void);
void wifi_mark_time_set(void);
void wifi_set_timezone(void);

/* Returns the device's WiFi MAC as hex, e.g. "543204470984".
 * Suitable for /api/esp/<device_id> endpoints. */
const char *wifi_get_device_id(void);

/* Set to true to keep the wifi event handler from auto-connecting to a
 * stale STA while the SoftAP is up (used during provisioning). The flag
 * affects WIFI_EVENT_STA_START and WIFI_EVENT_STA_DISCONNECTED handling. */
void wifi_suppress_auto_connect(bool suppress);

/* Mark the WiFi radio as started (called by start_softap after esp_wifi_start).
 * Keeps wifi.c's internal s_wifi_started flag in sync so a later
 * wifi_init_sta() call knows to stop+bounce the radio before switching to
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