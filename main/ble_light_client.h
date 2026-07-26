#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_LIGHT_STATUS_DISABLED = 0,
    BLE_LIGHT_STATUS_STARTING,
    BLE_LIGHT_STATUS_SCANNING,
    BLE_LIGHT_STATUS_CONNECTING,
    BLE_LIGHT_STATUS_CONNECTED,
    BLE_LIGHT_STATUS_RECONNECTING,
    BLE_LIGHT_STATUS_UNAVAILABLE,
} ble_light_status_t;

/* Reserve the BLE controller and start the NimBLE host task.  This does not
 * delay boot; it establishes and keeps a blinkwh connection in the background. */
esp_err_t ble_light_init(void);

/* Queue a non-blocking BLE request to turn on the configured WCH night
 * light. The implementation connects directly to its public address, writes
 * 0x01 to characteristic FFE1 on the persistent BLE connection. */
esp_err_t ble_light_turn_on(void);

/* Queue a non-blocking command to turn off the blinkwh night light. */
esp_err_t ble_light_turn_off(void);

/* Current state of the background blinkwh link.  Safe to poll from the
 * application task; it never drives the UI from the NimBLE host task. */
ble_light_status_t ble_light_get_status(void);

#ifdef __cplusplus
}
#endif
