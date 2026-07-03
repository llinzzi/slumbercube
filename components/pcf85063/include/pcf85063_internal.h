/* Internal helpers for PCF85063 — exposed for unit tests, do not use
 * from application code. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a decimal value 0-99 to BCD. */
uint8_t pcf85063_to_bcd(uint8_t val);

/* Convert a BCD value back to decimal. */
uint8_t pcf85063_from_bcd(uint8_t bcd);

/* True if the alarm field should be treated as wildcard. */
static inline bool pcf85063_alarm_field_disabled(uint8_t field)
{
    return (field & 0x80u) != 0u;
}

#ifdef __cplusplus
}
#endif
