#include "alarm_schedule.h"

#include <string.h>

bool alarm_schedule_next_occurrence(time_t now,
                                    int hour,
                                    int minute,
                                    bool allow_saturday,
                                    bool allow_sunday,
                                    alarm_occurrence_t *out)
{
    if (!out || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    struct tm local_now;
    if (!localtime_r(&now, &local_now)) return false;

    /* At least one weekday must remain enabled, and a seven-day search
     * covers the longest weekend-only restriction. */
    for (int day_offset = 0; day_offset < 8; day_offset++) {
        struct tm candidate = local_now;
        candidate.tm_mday += day_offset;
        candidate.tm_hour = hour;
        candidate.tm_min = minute;
        candidate.tm_sec = 0;
        candidate.tm_isdst = -1;

        time_t candidate_epoch = mktime(&candidate);
        if (candidate_epoch == (time_t)-1 || candidate_epoch <= now) {
            continue;
        }

        struct tm local_target;
        if (!localtime_r(&candidate_epoch, &local_target)) continue;
        if (local_target.tm_wday == 6 && !allow_saturday) continue;
        if (local_target.tm_wday == 0 && !allow_sunday) continue;

        struct tm utc_target;
        if (!gmtime_r(&candidate_epoch, &utc_target)) continue;

        out->local_epoch = candidate_epoch;
        out->local_year = local_target.tm_year + 1900;
        out->local_month = local_target.tm_mon + 1;
        out->local_day = local_target.tm_mday;
        out->local_hour = local_target.tm_hour;
        out->local_minute = local_target.tm_min;
        out->utc_weekday = (uint8_t)utc_target.tm_wday;
        return true;
    }

    return false;
}
