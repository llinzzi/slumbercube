/*
 * test_sys_fsm.c — 完整转换矩阵测试。
 * 直接链接生产 ../main/regions/sys_fsm.c。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/regions/sys_fsm.h"

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

static app_input_t mk_inp(void)
{
    app_input_t inp; memset(&inp, 0, sizeof(inp));
    return inp;
}

/* 标准 deep sleep 动作序列 (来自 main.c:1064-1102 旧实现) */
#define STANDARD_DEEP_SLEEP_ACTIONS \
    ACT_AUDIO_DEINIT, ACT_DISPLAY_OFF, ACT_GPIO_HOLD, ACT_TIMER_SET, ACT_DEEP_SLEEP

/* ── BOOT 转换 ────────────────────────────────────────────────────────── */
static void test_boot_done_to_normal(void)
{
    sys_state_t s = SYS_BOOT;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_BOOT_DONE, &inp);
    EXPECT(s == SYS_NORMAL);
    EXPECT(a.count == 0);
}

static void test_boot_deep_sleep_tick_rare_edge(void)
{
    /* 罕见边缘:冷启动中用户立即按右键配网超时 */
    sys_state_t s = SYS_BOOT;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_DEEP_SLEEP_TICK, &inp);
    EXPECT(s == SYS_SLEEPING);
    EXPECT_ACTIONS(a, STANDARD_DEEP_SLEEP_ACTIONS);
}

/* ── NORMAL 转换 ──────────────────────────────────────────────────────── */
static void test_normal_btn_sleep(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_BTN_SLEEP_PRESS, &inp);
    EXPECT(s == SYS_SLEEPING);
    EXPECT_ACTIONS(a, STANDARD_DEEP_SLEEP_ACTIONS);
}

static void test_normal_btn_factory_reset(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_BTN_FACTORY_RESET, &inp);
    EXPECT(s == SYS_SLEEPING);
    EXPECT_ACTIONS(a, ACT_NVS_ERASE, ACT_DEEP_SLEEP);
}

static void test_normal_prov_ok_reboot(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_PROV_OK, &inp);
    EXPECT(s == SYS_SLEEPING);
    EXPECT_ACTIONS(a, ACT_DEEP_SLEEP);
}

static void test_normal_prov_fail_stays(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_PROV_FAIL, &inp);
    EXPECT(s == SYS_NORMAL);
    EXPECT(a.count == 0);
}

static void test_normal_net_ok_stays(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_NET_OK, &inp);
    EXPECT(s == SYS_NORMAL);
    EXPECT(a.count == 0);
}

static void test_normal_tick_stays(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_TICK_1HZ, &inp);
    EXPECT(s == SYS_NORMAL);
    EXPECT(a.count == 0);
}

/* ── SLEEPING 终态 ────────────────────────────────────────────────────── */
static void test_sleeping_terminal(void)
{
    sys_state_t s = SYS_SLEEPING;
    app_input_t inp = mk_inp();
    /* 任何事件都不动 */
    sys_evt_t evts[] = {
        SYS_EVT_BOOT_DONE, SYS_EVT_BTN_SLEEP_PRESS, SYS_EVT_BTN_FACTORY_RESET,
        SYS_EVT_TICK_1HZ, SYS_EVT_NET_OK, SYS_EVT_PROV_OK, SYS_EVT_PROV_FAIL,
        SYS_EVT_DEEP_SLEEP_TICK,
    };
    for (size_t i = 0; i < sizeof(evts)/sizeof(evts[0]); i++) {
        fsm_actions_t a = sys_fsm_step(&s, evts[i], &inp);
        EXPECT(s == SYS_SLEEPING);
        EXPECT(a.count == 0);
    }
}

/* ── 不变量 ───────────────────────────────────────────────────────────── */
static void test_none_event_identity(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_NONE, &inp);
    EXPECT(s == SYS_NORMAL);
    EXPECT(a.count == 0);
}

static void test_actions_count_invariant(void)
{
    sys_state_t s = SYS_NORMAL;
    app_input_t inp = mk_inp();
    /* 标准 deep sleep 序列 = 5 个动作 */
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_BTN_SLEEP_PRESS, &inp);
    EXPECT(a.count <= FSM_ACTIONS_MAX);
    EXPECT(a.count == 5);  /* 文档化的固定序列 */
}

int main(void)
{
    test_boot_done_to_normal();
    test_boot_deep_sleep_tick_rare_edge();

    test_normal_btn_sleep();
    test_normal_btn_factory_reset();
    test_normal_prov_ok_reboot();
    test_normal_prov_fail_stays();
    test_normal_net_ok_stays();
    test_normal_tick_stays();

    test_sleeping_terminal();

    test_none_event_identity();
    test_actions_count_invariant();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok sys_fsm full transition matrix (%d cases)\n", 12);
    return 0;
}