/* BCD encode/decode for PCF85063 — kept in a separate translation unit
 * so unit tests can link against it without pulling in ESP-IDF headers.
 */
#include "pcf85063_internal.h"

uint8_t pcf85063_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10u) << 4) | (val % 10u));
}

uint8_t pcf85063_from_bcd(uint8_t bcd)
{
    return (uint8_t)((((bcd >> 4) & 0x0Fu) * 10u) + (bcd & 0x0Fu));
}
