/* PCF85063ATT/AJ real-time clock driver.
 * NXP PCF85063 — I2C address 0x51.
 * Reference: PCF85063 datasheet (rev 3.2, 2020-09-09).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock time exposed to the rest of the firmware.
 * `year` is the full year (e.g. 2026), not a 0-99 offset. */
typedef struct {
    uint16_t year;   /* 2000-2099 */
    uint8_t  month;  /* 1-12 */
    uint8_t  day;    /* 1-31 */
    uint8_t  weekday;/* 0-6 (PCF85063: 0=Sunday) */
    uint8_t  hour;   /* 0-23 (24h mode) */
    uint8_t  minute; /* 0-59 */
    uint8_t  second; /* 0-59 */
} pcf85063_datetime_t;

/* Alarm configuration. Each field is either a real value (0-59 for minute,
 * 0-23 for hour, 1-31 for day, 0-6 for weekday) or PCF85063_ALARM_DISABLE
 * to mark that field as "don't care". */
typedef struct {
    bool     enable;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  weekday;
} pcf85063_alarm_t;

#define PCF85063_ALARM_DISABLE  0x80u

/* Initialize the RTC over I2C and verify it ACKs.
 * Safe to call once at boot; returns ESP_ERR_INVALID_STATE on subsequent
 * calls without an intervening deinit. */
esp_err_t pcf85063_init(void);

/* Release the I2C bus. Call before re-init or before deep sleep if the
 * firmware needs to share the bus with another component. */
esp_err_t pcf85063_deinit(void);

/* Read the current date+time from the chip. */
esp_err_t pcf85063_read_datetime(pcf85063_datetime_t *dt);

/* Write date+time to the chip. The oscillator keeps running; this only
 * updates the timekeeping registers. */
esp_err_t pcf85063_set_datetime(const pcf85063_datetime_t *dt);

/* Configure the alarm. alarm->enable selects whether the alarm fires at
 * the (partial) match, and the fields marked PCF85063_ALARM_DISABLE are
 * treated as wildcards. The INT# pin (IO0) pulses low on match. */
esp_err_t pcf85063_set_alarm(const pcf85063_alarm_t *alarm);

/* Clear the alarm-flag bit in Control_2 so the next match can fire again. */
esp_err_t pcf85063_clear_alarm_flag(void);

/* Enable or disable the alarm interrupt output (Control_2.AIE). */
esp_err_t pcf85063_enable_alarm_int(bool enable);

/* Write the current system time (UTC) to the chip.
 * Used to sync the RTC after SNTP gives us a more accurate wall-clock.
 * Returns ESP_ERR_INVALID_STATE if init wasn't called or the chip didn't
 * respond. Returns ESP_ERR_INVALID_ARG if system time looks uninitialised. */
esp_err_t pcf85063_sync_from_system(void);

/* True if the chip responded to its I2C address during init. */
bool pcf85063_is_present(void);

#ifdef __cplusplus
}
#endif
