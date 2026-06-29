#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"

/* Outcome of a wifi_provisioning_run() call. */
typedef enum {
    WIFI_PROV_OK,            /* User submitted credentials; saved to NVS */
    WIFI_PROV_TIMEOUT,       /* Timed out without a submission */
    WIFI_PROV_ERROR,         /* Internal failure (AP/HTTP setup) */
} wifi_prov_result_t;

/* Spin up SoftAP + DNS redirect + captive-portal HTTP server, then block
 * (caller's task) until the user submits credentials OR the configured
 * timeout elapses. On return:
 *   - WIFI_PROV_OK: NVS now has valid credentials; caller may re-init STA
 *   - WIFI_PROV_TIMEOUT: no creds written; caller may fall back to menuconfig
 *   - WIFI_PROV_ERROR: setup failed (rare — heap/perm); no creds written
 *
 * This function assumes wifi_ensure_netif() has already been called by the
 * caller (it does NOT init netif itself, to avoid double-init with wifi.c).
 *
 * Safe to call at first boot (no NVS creds) and at runtime (after stopping
 * audio + STA). The function tears down its AP / HTTP / DNS resources
 * before returning, so the caller can hand control back to wifi_init_sta()
 * with the freshly-saved credentials.
 */
wifi_prov_result_t wifi_provisioning_run(void);

/* Stop an in-flight provisioning flow (e.g. when the user short-presses the
 * button to abort). Safe to call from any task. Idempotent. */
void wifi_provisioning_abort(void);

#endif // WIFI_PROVISIONING_H