#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve the BLE controller and start the NimBLE host task.  This does not
 * scan or connect; call it during boot while heap is still plentiful. */
esp_err_t ble_light_init(void);

/* Queue a non-blocking BLE request to turn on the paired WCH night light.
 * The implementation scans for the blinkwh FFE0 service, writes 0x01 to
 * characteristic FFE1, then releases the BLE connection. */
esp_err_t ble_light_turn_on(void);

/* Queue a non-blocking command to turn off the blinkwh night light. */
esp_err_t ble_light_turn_off(void);

#ifdef __cplusplus
}
#endif
