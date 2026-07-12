/*
 * test_net_fsm.c — 完整转换矩阵测试。
 * 直接链接生产 ../main/regions/net_fsm.c。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/regions/net_fsm.h"

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

/* ── OFFLINE 转换 ─────────────────────────────────────────────────────── */
static void test_offline_boot_done_with_creds(void)
{
    net_state_t s = NET_OFFLINE;
    app_input_t inp = mk_inp();
    inp.has_creds = true;
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_BOOT_DONE, &inp);
    EXPECT(s == NET_CONNECTING);
    EXPECT_ACTIONS(a, ACT_WIFI_INIT_STA);
}

static void test_offline_boot_done_without_creds(void)
{
    net_state_t s = NET_OFFLINE;
    app_input_t inp = mk_inp();
    inp.has_creds = false;
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_BOOT_DONE, &inp);
    EXPECT(s == NET_PROVISIONING);
    EXPECT_ACTIONS(a, ACT_RUN_PROVISIONING);
}

/* ── PROVISIONING 转换 ───────────────────────────────────────────────── */
static void test_provisioning_ok_to_offline(void)
{
    net_state_t s = NET_PROVISIONING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_PROV_OK, &inp);
    EXPECT(s == NET_OFFLINE);
    EXPECT(a.count == 0);  /* reboot 由 sys_fsm 处理 */
}

static void test_provisioning_fail_to_offline(void)
{
    net_state_t s = NET_PROVISIONING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_PROV_FAIL, &inp);
    EXPECT(s == NET_OFFLINE);
    EXPECT(a.count == 1);
    EXPECT(a.items[0].kind == ACT_DISPLAY_STATION);
    EXPECT(a.items[0].u.station.name != NULL);
}

static void test_provisioning_btn_request_reprovision(void)
{
    net_state_t s = NET_PROVISIONING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_BTN_REQUEST_PROVISION, &inp);
    EXPECT(s == NET_PROVISIONING);  /* stays */
    EXPECT_ACTIONS(a, ACT_RUN_PROVISIONING);
}

/* ── CONNECTING 转换 ─────────────────────────────────────────────────── */
static void test_connecting_ip_got_to_connected(void)
{
    net_state_t s = NET_CONNECTING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_IP_GOT, &inp);
    EXPECT(s == NET_CONNECTED);
    EXPECT_ACTIONS(a, ACT_NTP_START);
}

static void test_connecting_retry_exhausted_to_failed(void)
{
    net_state_t s = NET_CONNECTING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_WIFI_RETRY_EXHAUSTED, &inp);
    EXPECT(s == NET_FAILED);
    EXPECT_ACTIONS(a, ACT_NVS_ERASE_OLD_CREDS);
}

static void test_connecting_30s_timeout(void)
{
    net_state_t s = NET_CONNECTING;
    app_input_t inp = mk_inp();
    inp.net_connect_ticks = 30;
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_TICK_1HZ, &inp);
    EXPECT(s == NET_FAILED);
}

static void test_connecting_tick_below_threshold(void)
{
    net_state_t s = NET_CONNECTING;
    app_input_t inp = mk_inp();
    inp.net_connect_ticks = 5;
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_TICK_1HZ, &inp);
    EXPECT(s == NET_CONNECTING);
    EXPECT(a.count == 0);
}

/* ── CONNECTED 转换 ──────────────────────────────────────────────────── */
static void test_connected_disconnect_back_to_connecting(void)
{
    net_state_t s = NET_CONNECTED;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_STA_DISCONNECTED, &inp);
    EXPECT(s == NET_CONNECTING);
    EXPECT_ACTIONS(a, ACT_WIFI_RECONNECT);
}

/* ── FAILED 转换 ─────────────────────────────────────────────────────── */
static void test_failed_btn_request_provision(void)
{
    net_state_t s = NET_FAILED;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_BTN_REQUEST_PROVISION, &inp);
    EXPECT(s == NET_PROVISIONING);
    EXPECT_ACTIONS(a, ACT_RUN_PROVISIONING);
}

static void test_failed_tick_stays(void)
{
    net_state_t s = NET_FAILED;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_TICK_1HZ, &inp);
    EXPECT(s == NET_FAILED);
    EXPECT(a.count == 0);
}

/* ── 跨多状态:OFFLINE + 其他事件应保持 OFFLINE ────────────────────── */
static void test_offline_ignores_other_events(void)
{
    net_state_t s = NET_OFFLINE;
    app_input_t inp = mk_inp();
    inp.has_creds = true;
    net_evt_t evts[] = {
        NET_EVT_IP_GOT, NET_EVT_PROV_OK, NET_EVT_PROV_FAIL,
        NET_EVT_STA_DISCONNECTED, NET_EVT_TICK_1HZ,
    };
    for (size_t i = 0; i < sizeof(evts)/sizeof(evts[0]); i++) {
        fsm_actions_t a = net_fsm_step(&s, evts[i], &inp);
        EXPECT(s == NET_OFFLINE);
        EXPECT(a.count == 0);
    }
}

/* ── NONE 事件 ───────────────────────────────────────────────────────── */
static void test_none_event_identity(void)
{
    net_state_t s = NET_CONNECTED;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_NONE, &inp);
    EXPECT(s == NET_CONNECTED);
    EXPECT(a.count == 0);
}

static void test_actions_count_invariant(void)
{
    net_state_t s = NET_CONNECTING;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_IP_GOT, &inp);
    EXPECT(a.count <= FSM_ACTIONS_MAX);
}

/* ── 不变量:CONNECTED + IP_GOT 是 identity ─────────────────────────── */
static void test_connected_ignores_ip_got(void)
{
    net_state_t s = NET_CONNECTED;
    app_input_t inp = mk_inp();
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_IP_GOT, &inp);
    EXPECT(s == NET_CONNECTED);
    EXPECT(a.count == 0);
}

int main(void)
{
    test_offline_boot_done_with_creds();
    test_offline_boot_done_without_creds();

    test_provisioning_ok_to_offline();
    test_provisioning_fail_to_offline();
    test_provisioning_btn_request_reprovision();

    test_connecting_ip_got_to_connected();
    test_connecting_retry_exhausted_to_failed();
    test_connecting_30s_timeout();
    test_connecting_tick_below_threshold();

    test_connected_disconnect_back_to_connecting();
    test_connected_ignores_ip_got();

    test_failed_btn_request_provision();
    test_failed_tick_stays();

    test_offline_ignores_other_events();
    test_none_event_identity();
    test_actions_count_invariant();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok net_fsm full transition matrix (%d cases)\n", 16);
    return 0;
}