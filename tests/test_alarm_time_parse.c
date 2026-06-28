/* Host-runnable unit test for alarm_parse_time_string().
 * Compile with the Makefile in this directory.
 *
 * Exits non-zero if any assertion fails; prints PASS line per group.
 */
#include "alarm_time_parse.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do {                                                      \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        g_failures++;                                                         \
    }                                                                         \
} while (0)

#define CHECK_EQ_INT(actual, expected) do {                                  \
    g_checks++;                                                               \
    int _a = (int)(actual);                                                   \
    int _e = (int)(expected);                                                 \
    if (_a != _e) {                                                           \
        printf("  FAIL %s:%d: got %d, want %d\n",                             \
               __FILE__, __LINE__, _a, _e);                                   \
        g_failures++;                                                         \
    }                                                                         \
} while (0)

static void test_valid(void)
{
    printf("[test_valid]\n");
    uint8_t h = 0xFF, m = 0xFF;
    CHECK(alarm_parse_time_string("00:00", &h, &m));
    CHECK_EQ_INT(h, 0);
    CHECK_EQ_INT(m, 0);

    CHECK(alarm_parse_time_string("07:50", &h, &m));
    CHECK_EQ_INT(h, 7);
    CHECK_EQ_INT(m, 50);

    CHECK(alarm_parse_time_string("23:59", &h, &m));
    CHECK_EQ_INT(h, 23);
    CHECK_EQ_INT(m, 59);

    CHECK(alarm_parse_time_string("12:00", &h, &m));
    CHECK_EQ_INT(h, 12);
    CHECK_EQ_INT(m, 0);
}

static void test_out_of_range(void)
{
    printf("[test_out_of_range]\n");
    uint8_t h, m;
    /* hour > 23 */
    CHECK(!alarm_parse_time_string("24:00", &h, &m));
    CHECK(!alarm_parse_time_string("99:00", &h, &m));
    /* minute > 59 */
    CHECK(!alarm_parse_time_string("07:60", &h, &m));
    CHECK(!alarm_parse_time_string("23:99", &h, &m));
}

static void test_wrong_format(void)
{
    printf("[test_wrong_format]\n");
    uint8_t h, m;
    CHECK(!alarm_parse_time_string("", &h, &m));
    CHECK(!alarm_parse_time_string("0750", &h, &m));    /* no colon */
    CHECK(!alarm_parse_time_string("7:50", &h, &m));     /* not zero-padded */
    CHECK(!alarm_parse_time_string("07:5", &h, &m));     /* short minute */
    CHECK(!alarm_parse_time_string("07-50", &h, &m));    /* wrong separator */
    CHECK(!alarm_parse_time_string("07:50:00", &h, &m)); /* trailing */
    CHECK(!alarm_parse_time_string("07:50 ", &h, &m));   /* trailing space */
    CHECK(!alarm_parse_time_string(" 07:50", &h, &m));   /* leading space */
    CHECK(!alarm_parse_time_string("07:50\n", &h, &m));  /* trailing newline */
    CHECK(!alarm_parse_time_string("garbage", &h, &m));
    CHECK(!alarm_parse_time_string("07:ab", &h, &m));    /* non-digits */
    CHECK(!alarm_parse_time_string("ab:cd", &h, &m));
}

static void test_null_args(void)
{
    printf("[test_null_args]\n");
    uint8_t h = 0, m = 0;
    CHECK(!alarm_parse_time_string(NULL, &h, &m));
    CHECK(!alarm_parse_time_string("07:50", NULL, &m));
    CHECK(!alarm_parse_time_string("07:50", &h, NULL));
}

int main(void)
{
    test_valid();
    test_out_of_range();
    test_wrong_format();
    test_null_args();

    printf("\n[summary] %d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}