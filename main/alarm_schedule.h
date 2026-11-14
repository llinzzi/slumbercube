#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Local wall-clock time at which the alarm should fire. */
    time_t local_epoch;
    int    local_year;
    int    local_month;
    int    local_day;
    int    local_hour;
    int    local_minute;

    /* PCF85063 compares alarms against its UTC clock. */
    uint8_t utc_weekday;
} alarm_occurrence_t;

/* Find the first future occurrence of hour:minute that is allowed by the
 * weekend flags. The current minute has already elapsed, so a wake at
 * 07:30:07 schedules the next eligible day instead of re-arming 07:30 for
 * the same minute.
 *
 * Time zone must already be configured by the caller (SlumberCube uses
 * CST-8 / Asia/Shanghai). */
bool alarm_schedule_next_occurrence(time_t now,
                                    int hour,
                                    int minute,
                                    bool allow_saturday,
                                    bool allow_sunday,
                                    alarm_occurrence_t *out);

#ifdef __cplusplus
}
#endif
