/* Host-runnable unit test for PCF85063 BCD helpers.
 * Compile with the Makefile in this directory.
 *
 * Exits non-zero if any assertion fails; prints PASS line per group.
 */
#include "pcf85063_internal.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(actual, expected) do {                                       \
    g_checks++;                                                              \
    uint8_t _a = (uint8_t)(actual);                                          \
    uint8_t _e = (uint8_t)(expected);                                        \
    if (_a != _e) {                                                          \
        printf("  FAIL %s:%d: got 0x%02X, want 0x%02X\n",                    \
               __FILE__, __LINE__, _a, _e);                                  \
        g_failures++;                                                        \
    }                                                                        \
} while (0)

#define CHECK_TRUE(cond) do {                                                \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        g_failures++;                                                        \
    }                                                                        \
} while (0)

static void test_bcd_encode(void)
{
    printf("[test_bcd_encode]\n");
    CHECK_EQ(pcf85063_to_bcd(0),  0x00);
    CHECK_EQ(pcf85063_to_bcd(1),  0x01);
    CHECK_EQ(pcf85063_to_bcd(9),  0x09);
    CHECK_EQ(pcf85063_to_bcd(10), 0x10);
    CHECK_EQ(pcf85063_to_bcd(23), 0x23);
    CHECK_EQ(pcf85063_to_bcd(45), 0x45);
    CHECK_EQ(pcf85063_to_bcd(59), 0x59);
    CHECK_EQ(pcf85063_to_bcd(99), 0x99);
}

static void test_bcd_decode(void)
{
    printf("[test_bcd_decode]\n");
    CHECK_EQ(pcf85063_from_bcd(0x00),  0);
    CHECK_EQ(pcf85063_from_bcd(0x01),  1);
    CHECK_EQ(pcf85063_from_bcd(0x09),  9);
    CHECK_EQ(pcf85063_from_bcd(0x10), 10);
    CHECK_EQ(pcf85063_from_bcd(0x23), 23);
    CHECK_EQ(pcf85063_from_bcd(0x45), 45);
    CHECK_EQ(pcf85063_from_bcd(0x59), 59);
    CHECK_EQ(pcf85063_from_bcd(0x99), 99);
}

static void test_bcd_roundtrip(void)
{
    printf("[test_bcd_roundtrip]\n");
    /* Every valid seconds/minutes value 0-59 */
    for (int v = 0; v < 60; v++) {
        CHECK_EQ(pcf85063_from_bcd(pcf85063_to_bcd((uint8_t)v)), v);
    }
    /* Every valid hours value 0-23 */
    for (int v = 0; v < 24; v++) {
        CHECK_EQ(pcf85063_from_bcd(pcf85063_to_bcd((uint8_t)v)), v);
    }
    /* Every valid day value 1-31 */
    for (int v = 1; v < 32; v++) {
        CHECK_EQ(pcf85063_from_bcd(pcf85063_to_bcd((uint8_t)v)), v);
    }
    /* Every valid month value 1-12 */
    for (int v = 1; v < 13; v++) {
        CHECK_EQ(pcf85063_from_bcd(pcf85063_to_bcd((uint8_t)v)), v);
    }
    /* Every valid year value 0-99 */
    for (int v = 0; v < 100; v++) {
        CHECK_EQ(pcf85063_from_bcd(pcf85063_to_bcd((uint8_t)v)), v);
    }
}

static void test_alarm_field_disabled(void)
{
    printf("[test_alarm_field_disabled]\n");
    CHECK_TRUE(pcf85063_alarm_field_disabled(0x80));   /* spec value */
    CHECK_TRUE(pcf85063_alarm_field_disabled(0xFF));   /* all bits set, top still set */
    CHECK_TRUE(pcf85063_alarm_field_disabled(0x81));   /* spec value with low bit */
    CHECK_TRUE(!pcf85063_alarm_field_disabled(0x00));  /* 0 — no alarm disable flag */
    CHECK_TRUE(!pcf85063_alarm_field_disabled(0x59));  /* 59 (BCD) — not disabled */
    CHECK_TRUE(!pcf85063_alarm_field_disabled(0x7F));  /* just below threshold */
}

int main(void)
{
    test_bcd_encode();
    test_bcd_decode();
    test_bcd_roundtrip();
    test_alarm_field_disabled();

    if (g_failures == 0) {
        printf("\nPASS: %d checks\n", g_checks);
        return 0;
    }
    printf("\nFAIL: %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}