/*
 * event_router.c — 把原始 app_event_t 分发到 5 个 region 的子事件。
 *
 * 三种映射:
 *  1. 1-1 (大部分按钮事件): 只到一个 region
 *  2. 简单 fan-out (TICK_1HZ): 到所有 5 个 region
 *  3. 条件 fan-out (BOOT_DONE / ALARM_COMPLETE): 根据 s_state.wake 决定
 *     是否同时驱动 audio / sys
 *
 * 路由器是纯函数,可在主机上独立测试 (test_event_router.c)。
 */

#include "event_router.h"
#include <string.h>

static routed_events_t make_none(void)
{
    routed_events_t r;
    r.wake    = WAKE_EVT_NONE;
    r.sys     = SYS_EVT_NONE;
    r.net     = NET_EVT_NONE;
    r.audio   = AUDIO_EVT_NONE;
    r.display = DISP_EVT_NONE;
    return r;
}

routed_events_t route_event(app_event_t raw,
                             const app_state_t *state,
                             const app_input_t *inp)
{
    (void)inp;  /* 当前 router 不读 inp,纯 state-driven */

    routed_events_t r = make_none();

    switch (raw) {
    /* ── 一次性合成:boot 流程 ──────────────────────────────────────── */
    case EVT_WAKE_DETECT:
        /* 第一次进入主循环:把 wake_kind 转为 wake_fsm 事件。
         * 后续由 main.c 调用 wake_fsm_step(&s_state.wake, r.wake, &inp)
         * 触发 DORMANT -> FROM_BTN/RTC/SYS 转换。 */
        r.wake = WAKE_EVT_DETECT_SOURCE;
        break;

    case EVT_BOOT_DONE:
        /* sys 启动完成 -> NORMAL */
        r.sys = SYS_EVT_BOOT_DONE;
        /* net 启动决策 (有凭据 -> CONNECTING, 否则 -> PROVISIONING) */
        r.net = NET_EVT_BOOT_DONE;
        /* wake: 如果是 RTC 唤醒,fan-out 触发 ALARM_RINGING + auto-play。
         * 检测状态而非 inp.wake_kind 是因为 DETECT_SOURCE 已经把
         * wake_kind 翻译成 s_state.wake 的某个具体状态。 */
        if (state && state->wake == WAKE_FROM_RTC) {
            r.wake = WAKE_EVT_BOOT_DONE_FANOUT;
            /* audio: 闹钟唤醒强制 auto-play,无视 agent_enabled */
            r.audio = AUDIO_EVT_AUTO_PLAY_REQUEST;
        }
        break;

    /* ── 定时 ──────────────────────────────────────────────────────── */
    case EVT_TICK_1HZ:
        r.wake    = WAKE_EVT_TICK_1HZ;
        r.sys     = SYS_EVT_TICK_1HZ;
        r.net     = NET_EVT_TICK_1HZ;
        r.audio   = AUDIO_EVT_TICK_1HZ;
        r.display = DISP_EVT_TICK_1HZ;
        break;

    /* TICK_60S: 暂时只有 audio (室内传感器读取由 audio_fsm 决定)。
     * 未来如果其他 region 也用 60s 节拍,加在这里。 */
    case EVT_TICK_60S:
        /* 暂未给任何 region;保留以备扩展 */
        break;

    /* ── 按钮 ──────────────────────────────────────────────────────── */
    case EVT_BTN_SLEEP_PRESS:
        /* wake: FROM_BTN/ALARM_RINGING → GOTO_SLEEP */
        r.wake = WAKE_EVT_BTN_SLEEP_PRESS;
        /* sys: NORMAL → SLEEPING + deep sleep actions */
        r.sys  = SYS_EVT_BTN_SLEEP_PRESS;
        break;

    case EVT_BTN_NIGHT_TOGGLE:
        r.display = DISP_EVT_BTN_TOGGLE;
        break;

    case EVT_BTN_PROVISION_REQUEST:
        /* 既触发 wake 的 NO_CREDS (退化),也触发 net 的 BTN_REQUEST_PROVISION */
        r.wake = WAKE_EVT_NO_CREDS;
        r.net  = NET_EVT_BTN_REQUEST_PROVISION;
        break;

    case EVT_BTN_AUDIO_TOGGLE:
        r.audio = AUDIO_EVT_BTN_TOGGLE;
        break;

    case EVT_BTN_NTP_SYNC:
        /* 暂未分配 region:SNTP 同步由 net_fsm 内部处理 (TICK_1HZ 触发)
         * 或 wake_fsm 不直接管。如果未来需要给 audio 加 sync button,
         * 加在这里。 */
        break;

    case EVT_BTN_NEXT_TRACK:
        r.audio = AUDIO_EVT_BTN_NEXT;
        break;

    case EVT_BTN_LEFT_TOGGLE:
        /* 闹钟响铃时左键短按:只到 wake (关掉闹钟) */
        if (state && state->wake == WAKE_ALARM_RINGING) {
            r.wake = WAKE_EVT_BTN_LEFT_TOGGLE;
        } else {
            /* 非响铃状态:同 BTN_AUDIO_TOGGLE */
            r.audio = AUDIO_EVT_BTN_TOGGLE;
        }
        break;

    /* ── WiFi ──────────────────────────────────────────────────────── */
    case EVT_WIFI_STA_CONNECTED:
        r.net = NET_EVT_STA_CONNECTED;
        break;

    case EVT_WIFI_IP_GOT:
        r.net   = NET_EVT_IP_GOT;
        r.audio = AUDIO_EVT_NET_OK_FANOUT;
        break;

    case EVT_WIFI_DISCONNECTED:
        r.net = NET_EVT_STA_DISCONNECTED;
        break;

    case EVT_WIFI_TIMEOUT:
        r.net = NET_EVT_WIFI_RETRY_EXHAUSTED;
        break;

    /* ── 配网 ──────────────────────────────────────────────────────── */
    case EVT_PROVISION_OK:
        r.net = NET_EVT_PROV_OK;
        r.sys = SYS_EVT_PROV_OK;
        break;

    case EVT_PROVISION_FAIL:
        r.net = NET_EVT_PROV_FAIL;
        r.sys = SYS_EVT_PROV_FAIL;
        break;

    /* ── 音频播放器回调 ──────────────────────────────────────────────── */
    case EVT_AUDIO_PLAYER_PLAYING:
        r.audio = AUDIO_EVT_PLAYER_PLAYING;
        break;

    case EVT_AUDIO_PLAYER_IDLE:
        r.audio = AUDIO_EVT_PLAYER_IDLE;
        break;

    case EVT_AUDIO_PLAYER_NEXT:
        r.audio = AUDIO_EVT_PLAYER_NEXT;
        break;

    case EVT_AUDIO_PLAYER_ERROR:
        r.audio = AUDIO_EVT_PLAYER_ERROR;
        break;

    /* ── 闹钟完成 / deep sleep ─────────────────────────────────────── */
    case EVT_ALARM_COMPLETE:
        /* wake: ALARM_RINGING -> GOTO_SLEEP */
        r.wake  = WAKE_EVT_ALARM_COMPLETE;
        /* audio: PLAYING -> STOPPING */
        r.audio = AUDIO_EVT_ALARM_COMPLETE;
        /* sys: 进 deep sleep 流程 (即使 wake 还在 FROM_BTN) */
        r.sys   = SYS_EVT_DEEP_SLEEP_TICK;
        break;

    case EVT_DEEP_SLEEP_TICK:
        /* wake_fsm 已经转到 GOTO_SLEEP,通知 sys 走 deep sleep 流程 */
        r.sys = SYS_EVT_DEEP_SLEEP_TICK;
        break;

    /* ── 默认 ──────────────────────────────────────────────────────── */
    case EVT_NONE:
    default:
        /* identity: 所有 NONE */
        break;
    }

    return r;
}