/*
 * test_event_router.c — 完整路由表测试。
 * 直接链接生产 ../main/event_router.c + 5 个 region (后者只为链接需要,
 * 实际不被调用;路由表的正确性由 router 输出字段保证)。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/event_router.h"

static int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static app_state_t mk_state(int wake)
{
    app_state_t s;
    memset(&s, 0, sizeof(s));
    s.wake = wake;
    return s;
}

/* ── 1-1 按钮事件 ──────────────────────────────────────────────────── */
static void test_btn_sleep_press_to_wake(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_SLEEP_PRESS, &s, NULL);
    EXPECT(r.wake == WAKE_EVT_BTN_SLEEP_PRESS);
    EXPECT(r.sys == SYS_EVT_BTN_SLEEP_PRESS);
    EXPECT(r.net == NET_EVT_NONE);
    EXPECT(r.audio == AUDIO_EVT_NONE);
    EXPECT(r.display == DISP_EVT_NONE);
}

static void test_btn_night_toggle_to_display(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_NIGHT_TOGGLE, &s, NULL);
    EXPECT(r.display == DISP_EVT_BTN_TOGGLE);
    EXPECT(r.wake == WAKE_EVT_NONE);
}

static void test_btn_audio_toggle_to_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_AUDIO_TOGGLE, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_BTN_TOGGLE);
    EXPECT(r.wake == WAKE_EVT_NONE);
}

static void test_btn_next_track_to_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_NEXT_TRACK, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_BTN_NEXT);
}

static void test_btn_provision_request_to_wake_and_net(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_PROVISION_REQUEST, &s, NULL);
    EXPECT(r.wake == WAKE_EVT_NO_CREDS);
    EXPECT(r.net  == NET_EVT_BTN_REQUEST_PROVISION);
}

/* ── BTN_LEFT_TOGGLE 条件路由:响铃 vs 非响铃 ────────────────────── */
static void test_btn_left_toggle_when_alarm_ringing_to_wake(void)
{
    app_state_t s = mk_state(WAKE_ALARM_RINGING);
    routed_events_t r = route_event(EVT_BTN_LEFT_TOGGLE, &s, NULL);
    EXPECT(r.wake  == WAKE_EVT_BTN_LEFT_TOGGLE);
    EXPECT(r.audio == AUDIO_EVT_NONE);
}

static void test_btn_left_toggle_normal_to_audio(void)
{
    /* 非响铃:同 BTN_AUDIO_TOGGLE */
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BTN_LEFT_TOGGLE, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_BTN_TOGGLE);
    EXPECT(r.wake  == WAKE_EVT_NONE);
}

/* ── TICK_1HZ fan-out ─────────────────────────────────────────────── */
static void test_tick_1hz_fanout_all(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_TICK_1HZ, &s, NULL);
    EXPECT(r.wake    == WAKE_EVT_TICK_1HZ);
    EXPECT(r.sys     == SYS_EVT_TICK_1HZ);
    EXPECT(r.net     == NET_EVT_TICK_1HZ);
    EXPECT(r.audio   == AUDIO_EVT_TICK_1HZ);
    EXPECT(r.display == DISP_EVT_TICK_1HZ);
}

/* ── BOOT_DONE 条件 fan-out (基于 wake state) ───────────────────── */
static void test_boot_done_with_btn_wake(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_BOOT_DONE, &s, NULL);
    EXPECT(r.sys   == SYS_EVT_BOOT_DONE);
    EXPECT(r.net   == NET_EVT_BOOT_DONE);
    EXPECT(r.wake  == WAKE_EVT_NONE);  /* BTN wake 不触发 RTC fan-out */
    EXPECT(r.audio == AUDIO_EVT_NONE);
}

static void test_boot_done_with_rtc_wake_triggers_auto_play(void)
{
    /* 关键测试:RTC 唤醒 → wake BOOT_DONE_FANOUT + audio AUTO_PLAY_REQUEST */
    app_state_t s = mk_state(WAKE_FROM_RTC);
    routed_events_t r = route_event(EVT_BOOT_DONE, &s, NULL);
    EXPECT(r.sys   == SYS_EVT_BOOT_DONE);
    EXPECT(r.net   == NET_EVT_BOOT_DONE);
    EXPECT(r.wake  == WAKE_EVT_BOOT_DONE_FANOUT);
    EXPECT(r.audio == AUDIO_EVT_AUTO_PLAY_REQUEST);
}

static void test_boot_done_with_sys_wake(void)
{
    app_state_t s = mk_state(WAKE_FROM_SYS);
    routed_events_t r = route_event(EVT_BOOT_DONE, &s, NULL);
    EXPECT(r.wake  == WAKE_EVT_NONE);  /* SYS wake 不触发 RTC fan-out */
    EXPECT(r.audio == AUDIO_EVT_NONE);
}

/* ── WAKE_DETECT 第一次 boot ────────────────────────────────────── */
static void test_wake_detect_only_to_wake(void)
{
    app_state_t s = mk_state(WAKE_DORMANT);
    routed_events_t r = route_event(EVT_WAKE_DETECT, &s, NULL);
    EXPECT(r.wake == WAKE_EVT_DETECT_SOURCE);
    EXPECT(r.sys == SYS_EVT_NONE);
}

/* ── WiFi IP_GOT fan-out ────────────────────────────────────────── */
static void test_wifi_ip_got_to_net_and_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_WIFI_IP_GOT, &s, NULL);
    EXPECT(r.net   == NET_EVT_IP_GOT);
    EXPECT(r.audio == AUDIO_EVT_NET_OK_FANOUT);
}

static void test_wifi_sta_connected_only_to_net(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_WIFI_STA_CONNECTED, &s, NULL);
    EXPECT(r.net   == NET_EVT_STA_CONNECTED);
    EXPECT(r.audio == AUDIO_EVT_NONE);
}

static void test_wifi_disconnected_to_net(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_WIFI_DISCONNECTED, &s, NULL);
    EXPECT(r.net == NET_EVT_STA_DISCONNECTED);
}

/* ── Provision 结果 ──────────────────────────────────────────────── */
static void test_prov_ok_to_net_and_sys(void)
{
    app_state_t s = mk_state(WAKE_FROM_SYS);
    routed_events_t r = route_event(EVT_PROVISION_OK, &s, NULL);
    EXPECT(r.net == NET_EVT_PROV_OK);
    EXPECT(r.sys == SYS_EVT_PROV_OK);
}

static void test_prov_fail_to_net_and_sys(void)
{
    app_state_t s = mk_state(WAKE_FROM_SYS);
    routed_events_t r = route_event(EVT_PROVISION_FAIL, &s, NULL);
    EXPECT(r.net == NET_EVT_PROV_FAIL);
    EXPECT(r.sys == SYS_EVT_PROV_FAIL);
}

/* ── 音频回调 1-1 ────────────────────────────────────────────────── */
static void test_audio_player_playing_to_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_AUDIO_PLAYER_PLAYING, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_PLAYER_PLAYING);
    EXPECT(r.wake == WAKE_EVT_NONE);
}

static void test_audio_player_idle_to_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_AUDIO_PLAYER_IDLE, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_PLAYER_IDLE);
}

static void test_audio_player_error_to_audio(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_AUDIO_PLAYER_ERROR, &s, NULL);
    EXPECT(r.audio == AUDIO_EVT_PLAYER_ERROR);
}

/* ── 闹钟完成 fan-out (关键) ─────────────────────────────────────── */
static void test_alarm_complete_fanout(void)
{
    app_state_t s = mk_state(WAKE_ALARM_RINGING);
    routed_events_t r = route_event(EVT_ALARM_COMPLETE, &s, NULL);
    EXPECT(r.wake  == WAKE_EVT_ALARM_COMPLETE);
    EXPECT(r.audio == AUDIO_EVT_ALARM_COMPLETE);
    EXPECT(r.sys   == SYS_EVT_DEEP_SLEEP_TICK);
    EXPECT(r.net   == NET_EVT_NONE);
}

/* ── Deep sleep tick 单独路由 ─────────────────────────────────────── */
static void test_deep_sleep_tick_only_to_sys(void)
{
    app_state_t s = mk_state(WAKE_GOTO_SLEEP);
    routed_events_t r = route_event(EVT_DEEP_SLEEP_TICK, &s, NULL);
    EXPECT(r.sys == SYS_EVT_DEEP_SLEEP_TICK);
}

/* ── 不识别的 raw event → 全部 NONE ────────────────────────────── */
static void test_unknown_event_all_none(void)
{
    app_state_t s = mk_state(WAKE_FROM_BTN);
    routed_events_t r = route_event(EVT_NONE, &s, NULL);
    EXPECT(r.wake    == WAKE_EVT_NONE);
    EXPECT(r.sys     == SYS_EVT_NONE);
    EXPECT(r.net     == NET_EVT_NONE);
    EXPECT(r.audio   == AUDIO_EVT_NONE);
    EXPECT(r.display == DISP_EVT_NONE);
}

/* ── NULL state 也安全 (默认值 boot) ────────────────────────────── */
static void test_null_state_safe(void)
{
    routed_events_t r = route_event(EVT_TICK_1HZ, NULL, NULL);
    EXPECT(r.wake    == WAKE_EVT_TICK_1HZ);
    /* boot_done with NULL state: 不触发 RTC fan-out */
    r = route_event(EVT_BOOT_DONE, NULL, NULL);
    EXPECT(r.wake == WAKE_EVT_NONE);
}

int main(void)
{
    test_btn_sleep_press_to_wake();
    test_btn_night_toggle_to_display();
    test_btn_audio_toggle_to_audio();
    test_btn_next_track_to_audio();
    test_btn_provision_request_to_wake_and_net();
    test_btn_left_toggle_when_alarm_ringing_to_wake();
    test_btn_left_toggle_normal_to_audio();

    test_tick_1hz_fanout_all();

    test_boot_done_with_btn_wake();
    test_boot_done_with_rtc_wake_triggers_auto_play();
    test_boot_done_with_sys_wake();
    test_wake_detect_only_to_wake();

    test_wifi_ip_got_to_net_and_audio();
    test_wifi_sta_connected_only_to_net();
    test_wifi_disconnected_to_net();

    test_prov_ok_to_net_and_sys();
    test_prov_fail_to_net_and_sys();

    test_audio_player_playing_to_audio();
    test_audio_player_idle_to_audio();
    test_audio_player_error_to_audio();

    test_alarm_complete_fanout();
    test_deep_sleep_tick_only_to_sys();

    test_unknown_event_all_none();
    test_null_state_safe();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok event_router full fan-out table (%d cases)\n", 24);
    return 0;
}