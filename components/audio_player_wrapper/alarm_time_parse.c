#include "alarm_time_parse.h"
#include <stdio.h>

bool alarm_parse_time_string(const char *s, uint8_t *hour, uint8_t *minute)
{
    if (!s || !hour || !minute) return false;

    /* Strict format: exactly 5 characters "DD:DD" plus a terminating NUL.
     * Length check rejects "7:50", "07:5", "07:50:00", "07:50 ", etc. */
    char c0, c1, c2, c3, c4;
    int matched = sscanf(s, "%c%c%c%c%c", &c0, &c1, &c2, &c3, &c4);
    if (matched != 5) return false;          /* too short / NULL / empty */
    if (s[5] != '\0') return false;          /* trailing content present */
    if (c2 != ':') return false;             /* colon in wrong place */
    if (c0 < '0' || c0 > '9') return false;
    if (c1 < '0' || c1 > '9') return false;
    if (c3 < '0' || c3 > '9') return false;
    if (c4 < '0' || c4 > '9') return false;

    int h = (c0 - '0') * 10 + (c1 - '0');
    int m = (c3 - '0') * 10 + (c4 - '0');
    if (h > 23 || m > 59) return false;

    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    return true;
}