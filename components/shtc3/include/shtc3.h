#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize SHTC3 sensor on the configured I2C pins.
 * Returns true if the sensor responded (ID register read OK).
 * Safe to call once at boot; sensor-less variants simply return false. */
bool shtc3_init(void);

/* Wake, take a single T+RH measurement, sleep.
 * Returns true on success and fills *temp_c (°C) and *humidity (%RH).
 * Returns false if init was never successful or the read fails. */
bool shtc3_read(float *temp_c, float *humidity);

#ifdef __cplusplus
}
#endif