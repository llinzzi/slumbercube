/*
 * test_display_fsm.c — 完整转换矩阵测试。
 * 直接链接生产 ../main/regions/display_fsm.c。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/regions/display_fsm.h"

static int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define EXPECT_ACTIONS(a, ...) do { \
    app_action_kind_t expected[] = { __VA_ARGS__ }; \
    size_t n = sizeof(expected) / sizeof(expected[0]); \
    EXPECT((a).count == n); \
    for (size_t i = 0; i < n && i < (a).count; i++) { \
        EXPECT((a).items[i].kind == expected[i]); \
    } \
} while (0)

static bool contains(const fsm_actions_t *a, app_action_kind_t k)
{
    for (uint8_t i = 0; i < a->count; i++) if (a->items[i].kind == k) return true;
    return false;
}

static app_input_t mk_inp(bool night_now)
{
    app_input_t inp; memset(&inp, 0, sizeof(inp));
    inp.night_now = night_now;
    return inp;
}

/* ── DAY 转换 ────────────────────────────────────────────────────────── */
static void test_day_tick_entering_night(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(true);  /* 现在是夜间 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(s == DISP_NIGHT_AUTO);
    EXPECT_ACTIONS(a, ACT_SET_NIGHT_MODE, ACT_DRAW_MINIMAL_CLOCK);
    /* night_mode = true */
    EXPECT(a.items[0].u.night.on == true);
}

static void test_day_tick_still_day(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(false);  /* 仍是白天 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(s == DISP_DAY);
    EXPECT(a.count == 0);
}

static void test_day_btn_toggle_to_forced(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(false);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_NIGHT_FORCED);
    EXPECT(contains(&a, ACT_SET_NIGHT_OVERRIDE));
    EXPECT(a.items[0].u.night_override.override == 1);
    EXPECT(contains(&a, ACT_SET_NIGHT_MODE));
    EXPECT(a.items[1].u.night.on == true);
}

/* ── NIGHT_AUTO 转换 ─────────────────────────────────────────────────── */
static void test_night_auto_tick_leaving_night(void)
{
    display_state_t s = DISP_NIGHT_AUTO;
    app_input_t inp = mk_inp(false);  /* 离开夜间 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(s == DISP_DAY);
    EXPECT_ACTIONS(a, ACT_SET_NIGHT_MODE, ACT_DRAW_WEATHER);
    EXPECT(a.items[0].u.night.on == false);
}

static void test_night_auto_tick_still_night(void)
{
    display_state_t s = DISP_NIGHT_AUTO;
    app_input_t inp = mk_inp(true);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(s == DISP_NIGHT_AUTO);
    EXPECT(a.count == 0);
}

static void test_night_auto_btn_toggle_to_forced(void)
{
    display_state_t s = DISP_NIGHT_AUTO;
    app_input_t inp = mk_inp(true);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_NIGHT_FORCED);
    EXPECT(a.items[0].u.night_override.override == 1);
}

/* ── NIGHT_FORCED 转换 ───────────────────────────────────────────────── */
static void test_forced_btn_toggle_during_night_returns_to_auto(void)
{
    display_state_t s = DISP_NIGHT_FORCED;
    app_input_t inp = mk_inp(true);  /* 当前仍是夜间时段 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_NIGHT_AUTO);
    EXPECT(a.items[0].u.night_override.override == -1);
}

static void test_forced_btn_toggle_during_day_returns_to_day(void)
{
    display_state_t s = DISP_NIGHT_FORCED;
    app_input_t inp = mk_inp(false);  /* 已经白天 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_DAY);
    EXPECT(a.items[0].u.night_override.override == 0);
    EXPECT(contains(&a, ACT_SET_NIGHT_MODE));
    /* night_mode = false (回 DAY 时关闭夜间) */
    bool found_night_off = false;
    for (uint8_t i = 0; i < a.count; i++) {
        if (a.items[i].kind == ACT_SET_NIGHT_MODE) {
            EXPECT(!a.items[i].u.night.on);
            found_night_off = true;
        }
    }
    EXPECT(found_night_off);
}

static void test_forced_tick_does_not_transition(void)
{
    /* 强制模式不响应时间穿越,等用户取消 */
    display_state_t s = DISP_NIGHT_FORCED;
    app_input_t inp = mk_inp(false);  /* 已经白天 */
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(s == DISP_NIGHT_FORCED);
    EXPECT(a.count == 0);
}

/* ── 不变量 ───────────────────────────────────────────────────────────── */
static void test_none_event_identity(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(false);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_NONE, &inp);
    EXPECT(s == DISP_DAY);
    EXPECT(a.count == 0);
}

static void test_agent_off_event_identity(void)
{
    /* AGENT_OFF 是 informational,不应引发任何 transition */
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(false);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_AGENT_OFF, &inp);
    EXPECT(s == DISP_DAY);
    EXPECT(a.count == 0);
}

static void test_actions_count_invariant(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(true);
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    EXPECT(a.count <= FSM_ACTIONS_MAX);
}

/* ── 完整循环:DAY → FORCED → DAY (白天时段) ────────────────────── */
static void test_full_cycle_day_to_forced_to_day(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = mk_inp(false);

    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_NIGHT_FORCED);
    (void)a;

    fsm_actions_t a2 = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    EXPECT(s == DISP_DAY);
    (void)a2;
}

int main(void)
{
    test_day_tick_entering_night();
    test_day_tick_still_day();
    test_day_btn_toggle_to_forced();

    test_night_auto_tick_leaving_night();
    test_night_auto_tick_still_night();
    test_night_auto_btn_toggle_to_forced();

    test_forced_btn_toggle_during_night_returns_to_auto();
    test_forced_btn_toggle_during_day_returns_to_day();
    test_forced_tick_does_not_transition();

    test_none_event_identity();
    test_agent_off_event_identity();
    test_actions_count_invariant();

    test_full_cycle_day_to_forced_to_day();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok display_fsm full transition matrix (%d cases)\n", 13);
    return 0;
}