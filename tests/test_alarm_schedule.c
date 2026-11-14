#define _POSIX_C_SOURCE 200809L

#include "alarm_schedule.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static time_t local_epoch(int year, int month, int day, int hour, int minute, int second)
{
    struct tm value;
    memset(&value, 0, sizeof(value));
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    return mktime(&value);
}

static int local_weekday(time_t value)
{
    struct tm local;
    assert(localtime_r(&value, &local));
    return local.tm_wday;
}

static void test_saturday_is_skipped(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 14, 8, 0, 0); /* Saturday */

    assert(alarm_schedule_next_occurrence(now, 7, 30, false, false, &next));
    assert(next.local_year == 2026);
    assert(next.local_month == 11);
    assert(next.local_day == 16); /* Monday */
    assert(local_weekday(next.local_epoch) == 1);
}

static void test_sunday_is_skipped(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 15, 8, 0, 0); /* Sunday */

    assert(alarm_schedule_next_occurrence(now, 7, 30, false, true, &next));
    assert(next.local_day == 16);
    assert(local_weekday(next.local_epoch) == 1);
}

static void test_current_minute_is_not_rearmed(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 16, 7, 30, 7); /* Monday */

    assert(alarm_schedule_next_occurrence(now, 7, 30, true, true, &next));
    assert(next.local_day == 17);
    assert(next.local_epoch > now + 23 * 60 * 60);
}

static void test_later_today_is_allowed(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 16, 7, 0, 0);

    assert(alarm_schedule_next_occurrence(now, 7, 30, true, true, &next));
    assert(next.local_day == 16);
    assert(next.local_hour == 7);
    assert(next.local_minute == 30);
}

static void test_utc_weekday_is_used(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 14, 8, 0, 0);

    assert(alarm_schedule_next_occurrence(now, 7, 30, false, false, &next));

    struct tm utc;
    assert(gmtime_r(&next.local_epoch, &utc));
    assert(next.utc_weekday == utc.tm_wday);
    /* Monday 07:30 CST is Sunday 23:30 UTC. */
    assert(next.utc_weekday == 0);
}

static void test_invalid_inputs_are_rejected(void)
{
    alarm_occurrence_t next;
    time_t now = local_epoch(2026, 11, 14, 8, 0, 0);

    assert(!alarm_schedule_next_occurrence(now, 24, 0, true, true, &next));
    assert(!alarm_schedule_next_occurrence(now, 7, 60, true, true, &next));
    assert(!alarm_schedule_next_occurrence(now, 7, 30, true, true, NULL));
}

int main(void)
{
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();

    printf("test_alarm_schedule:\n");
    test_saturday_is_skipped();
    printf("  ok  Saturday alarm skips to Monday\n");
    test_sunday_is_skipped();
    printf("  ok  disabled Sunday skips to Monday\n");
    test_current_minute_is_not_rearmed();
    printf("  ok  current alarm minute schedules the next day\n");
    test_later_today_is_allowed();
    printf("  ok  a future alarm later today remains eligible\n");
    test_utc_weekday_is_used();
    printf("  ok  RTC weekday comes from UTC time\n");
    test_invalid_inputs_are_rejected();
    printf("  ok  invalid inputs are rejected\n");
    printf("all tests passed\n");
    return 0;
}
