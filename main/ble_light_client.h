#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve the BLE controller and start the NimBLE host task.  This does not
 * delay boot; it establishes and keeps a blinkwh connection in the background. */
esp_err_t ble_light_init(void);

/* Queue a non-blocking BLE request to turn on the configured WCH night
 * light. The implementation connects directly to its public address, writes
 * 0x01 to characteristic FFE1 on the persistent BLE connection. */
esp_err_t ble_light_turn_on(void);

/* Queue a non-blocking command to turn off the blinkwh night light. */
esp_err_t ble_light_turn_off(void);

#ifdef __cplusplus
}
#endif
