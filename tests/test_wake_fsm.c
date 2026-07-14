/*
 * test_wake_fsm.c — 完整转换矩阵测试。
 *
 * 按 plan 文档 "测试矩阵 -> wake_fsm" 章节的覆盖率要求。
 * 直接链接生产 ../main/regions/wake_fsm.c,生产代码漂移会编译期暴露。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/regions/wake_fsm.h"

static int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 检查 actions 列表:count + 各 kind 按顺序 */
#define EXPECT_ACTIONS(a, ...) do { \
    app_action_kind_t expected[] = { __VA_ARGS__ }; \
    size_t n = sizeof(expected) / sizeof(expected[0]); \
    EXPECT((a).count == n); \
    for (size_t i = 0; i < n && i < (a).count; i++) { \
        EXPECT((a).items[i].kind == expected[i]); \
    } \
} while (0)

/* 便利构造:零初始化的 app_input_t */
static app_input_t mk_inp(void)
{
    app_input_t inp;
    memset(&inp, 0, sizeof(inp));
    return inp;
}

/* ── DORMANT + DETECT_SOURCE 三种分支 ─────────────────────────────────── */
static void test_dormant_detect_btn(void)
{
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = mk_inp();
    inp.wake_kind = WAKE_BTN;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT_ACTIONS(a, ACT_DISPLAY_FADE_IN);
}

static void test_dormant_detect_rtc(void)
{
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = mk_inp();
    inp.wake_kind = WAKE_RTC;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    EXPECT(s == WAKE_FROM_RTC);
    EXPECT_ACTIONS(a, ACT_DISPLAY_FADE_IN, ACT_VOLUME_MAX, ACT_NET_AUTO_CONNECT);
}

static void test_dormant_detect_sys(void)
{
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = mk_inp();
    inp.wake_kind = WAKE_SYS;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    EXPECT(s == WAKE_FROM_SYS);
    EXPECT(a.count == 0);
}

static void test_dormant_detect_none(void)
{
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = mk_inp();
    inp.wake_kind = WAKE_NONE;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    EXPECT(s == WAKE_FROM_SYS);
}

/* ── FROM_BTN 转换 ────────────────────────────────────────────────────── */
static void test_from_btn_no_creds(void)
{
    wake_state_t s = WAKE_FROM_BTN;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_NO_CREDS, &inp);
    EXPECT(s == WAKE_FROM_SYS);
    EXPECT(a.count == 0);
}

static void test_from_btn_sleep_press(void)
{
    wake_state_t s = WAKE_FROM_BTN;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BTN_SLEEP_PRESS, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT_ACTIONS(a, ACT_DISPLAY_OFF, ACT_DEEP_SLEEP);
}

static void test_from_btn_tick_stays(void)
{
    wake_state_t s = WAKE_FROM_BTN;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_TICK_1HZ, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT(a.count == 0);
}

/* 关键不变量:FROM_BTN 绝不发出 ACT_NET_AUTO_CONNECT 或 ACT_AUDIO_AUTO_PLAY */
static void test_from_btn_invariant_no_auto_play(void)
{
    /* 在所有可能的 (state, evt) 组合下,FROM_BTN 发出的动作不含这两种 */
    wake_evt_t evts[] = {
        WAKE_EVT_DETECT_SOURCE, WAKE_EVT_BTN_SLEEP_PRESS,
        WAKE_EVT_BTN_LEFT_TOGGLE, WAKE_EVT_ALARM_COMPLETE,
        WAKE_EVT_NO_CREDS, WAKE_EVT_BOOT_DONE_FANOUT,
        WAKE_EVT_TICK_1HZ,
    };
    for (size_t i = 0; i < sizeof(evts)/sizeof(evts[0]); i++) {
        wake_state_t s = WAKE_FROM_BTN;
        app_input_t inp = mk_inp();
        inp.alarm_ring_minutes = 99;
        inp.weekend_skip = true;
        fsm_actions_t a = wake_fsm_step(&s, evts[i], &inp);
        for (uint8_t k = 0; k < a.count; k++) {
            EXPECT(a.items[k].kind != ACT_NET_AUTO_CONNECT);
            EXPECT(a.items[k].kind != ACT_AUDIO_AUTO_PLAY);
        }
    }
}

/* ── FROM_RTC 转换 ────────────────────────────────────────────────────── */
static void test_from_rtc_no_creds_degrades_to_btn(void)
{
    wake_state_t s = WAKE_FROM_RTC;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_NO_CREDS, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT(a.count == 0);
}

static void test_from_rtc_boot_done_weekend_degrades(void)
{
    wake_state_t s = WAKE_FROM_RTC;
    app_input_t inp = mk_inp();
    inp.weekend_skip = true;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BOOT_DONE_FANOUT, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT(a.count == 0);
}

static void test_from_rtc_boot_done_normal_to_alarm(void)
{
    wake_state_t s = WAKE_FROM_RTC;
    app_input_t inp = mk_inp();
    inp.weekend_skip = false;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BOOT_DONE_FANOUT, &inp);
    EXPECT(s == WAKE_ALARM_RINGING);
    EXPECT_ACTIONS(a, ACT_AUDIO_AUTO_PLAY);
}

static void test_from_rtc_sleep_press(void)
{
    wake_state_t s = WAKE_FROM_RTC;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BTN_SLEEP_PRESS, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT_ACTIONS(a, ACT_DISPLAY_OFF, ACT_DEEP_SLEEP);
}

/* ── ALARM_RINGING 三种退出路径 ───────────────────────────────────────── */
static void test_alarm_ringing_btn_left_to_btn(void)
{
    wake_state_t s = WAKE_ALARM_RINGING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BTN_LEFT_TOGGLE, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT_ACTIONS(a, ACT_AUDIO_STOP);
}

static void test_alarm_ringing_alarm_complete(void)
{
    wake_state_t s = WAKE_ALARM_RINGING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_ALARM_COMPLETE, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT_ACTIONS(a, ACT_ARM_RTC_FOR_TOMORROW, ACT_DEEP_SLEEP);
}

static void test_alarm_ringing_15min_timeout(void)
{
    wake_state_t s = WAKE_ALARM_RINGING;
    app_input_t inp = mk_inp();
    inp.alarm_ring_minutes = 15;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_TICK_1HZ, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT_ACTIONS(a, ACT_ARM_RTC_FOR_TOMORROW, ACT_DEEP_SLEEP);
}

static void test_alarm_ringing_tick_below_threshold(void)
{
    wake_state_t s = WAKE_ALARM_RINGING;
    app_input_t inp = mk_inp();
    inp.alarm_ring_minutes = 5;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_TICK_1HZ, &inp);
    EXPECT(s == WAKE_ALARM_RINGING);
    EXPECT(a.count == 0);
}

/* ── 终态 ────────────────────────────────────────────────────────────── */
static void test_goto_sleep_terminal(void)
{
    wake_state_t s = WAKE_GOTO_SLEEP;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_BTN_SLEEP_PRESS, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT(a.count == 0);

    a = wake_fsm_step(&s, WAKE_EVT_TICK_1HZ, &inp);
    EXPECT(s == WAKE_GOTO_SLEEP);
    EXPECT(a.count == 0);
}

static void test_from_sys_no_transitions(void)
{
    wake_state_t s = WAKE_FROM_SYS;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_TICK_1HZ, &inp);
    EXPECT(s == WAKE_FROM_SYS);
    EXPECT(a.count == 0);
}

/* ── NONE 事件早返回 identity ─────────────────────────────────────────── */
static void test_none_event_identity(void)
{
    wake_state_t s = WAKE_FROM_BTN;
    app_input_t inp = mk_inp();
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_NONE, &inp);
    EXPECT(s == WAKE_FROM_BTN);
    EXPECT(a.count == 0);
}

/* ── actions 不超过上限 ──────────────────────────────────────────────── */
static void test_actions_count_invariant(void)
{
    /* 触发多个动作的转换:任何转换的 out.count 都 <= FSM_ACTIONS_MAX */
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = mk_inp();
    inp.wake_kind = WAKE_RTC;
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    EXPECT(a.count <= FSM_ACTIONS_MAX);
}

int main(void)
{
    test_dormant_detect_btn();
    test_dormant_detect_rtc();
    test_dormant_detect_sys();
    test_dormant_detect_none();

    test_from_btn_no_creds();
    test_from_btn_sleep_press();
    test_from_btn_tick_stays();
    test_from_btn_invariant_no_auto_play();

    test_from_rtc_no_creds_degrades_to_btn();
    test_from_rtc_boot_done_weekend_degrades();
    test_from_rtc_boot_done_normal_to_alarm();
    test_from_rtc_sleep_press();

    test_alarm_ringing_btn_left_to_btn();
    test_alarm_ringing_alarm_complete();
    test_alarm_ringing_15min_timeout();
    test_alarm_ringing_tick_below_threshold();

    test_goto_sleep_terminal();
    test_from_sys_no_transitions();

    test_none_event_identity();
    test_actions_count_invariant();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok wake_fsm full transition matrix (%d cases)\n",
           20 /* count of tests above */);
    return 0;
}