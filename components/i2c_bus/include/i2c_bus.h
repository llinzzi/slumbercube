/* Shared I2C master bus for SHTC3 + PCF85063 (and any future I2C devices).
 *
 * Owns one ESP-IDF i2c_master_bus handle. Components call i2c_bus_get() to
 * obtain the handle, then attach their own device via
 * i2c_master_bus_add_device(). Do not call i2c_del_master_bus() directly —
 * use i2c_bus_deinit() so the reference count stays consistent.
 */
#pragma once

#include <stdbool.h>
#include <driver/i2c_master.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the shared I2C bus on first call. Idempotent.
 * Returns ESP_OK on success, error code otherwise. */
esp_err_t i2c_bus_init(void);

/* Get the bus handle. Returns NULL if i2c_bus_init() has not been called. */
i2c_master_bus_handle_t i2c_bus_get(void);

/* Tear down the shared bus. All devices must already be removed
 * (i2c_master_bus_rm_device) by their owners before calling this. */
esp_err_t i2c_bus_deinit(void);

/* True if i2c_bus_init() succeeded at least once and the bus is live. */
bool i2c_bus_is_ready(void);

#ifdef __cplusplus
}
#endif
