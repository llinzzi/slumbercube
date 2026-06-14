#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_init_sta(void);
esp_err_t wifi_ensure_netif(void);
bool wifi_is_connected(void);
bool wifi_is_time_set(void);
void wifi_mark_time_set(void);
void wifi_set_timezone(void);

/* Returns the device's WiFi MAC as hex, e.g. "543204470984".
 * Suitable for /api/esp/<device_id> endpoints. */
const char *wifi_get_device_id(void);

#endif // WIFI_MANAGER_H
