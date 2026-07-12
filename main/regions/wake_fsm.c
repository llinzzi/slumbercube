/*
 * wake_fsm.c — 唤醒来源与唤醒后生命周期 FSM。
 *
 * 完整转换矩阵见 plan 文档 "1. wake_fsm" 章节。
 *
 * 关键不变量:
 *  - FROM_BTN 状态绝不发出 ACT_NET_AUTO_CONNECT 或 ACT_AUDIO_AUTO_PLAY
 *    (用户按键唤醒不该自动连网或自动播放)
 *  - GOTO_SLEEP 是终态前的过渡,executor 看到 ACT_DEEP_SLEEP 立即 deep sleep
 *  - ALARM_RINGING 的 15min 兜底由 alarm_ring_minutes 计时器触发
 *    (executor 在 ALARM_RINGING entry 时把 alarm_ring_minutes 复位为 0)
 */

#include "wake_fsm.h"
#include <string.h>

/* 把动作 append 到 out (最多 FSM_ACTIONS_MAX 项)。当前 wake_fsm 只发
 * kind-only 的动作(没有 payload),所以 helper 不需要 set union。 */
static fsm_actions_t add_action(fsm_actions_t a, app_action_kind_t kind)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = kind;
        /* 留白 union (所有 payload 都初始化为 0) */
        memset(&a.items[a.count].u, 0, sizeof(a.items[a.count].u));
        a.count++;
    }
    return a;
}

fsm_actions_t wake_fsm_step(wake_state_t *cur, wake_evt_t evt, const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };

    /* NONE 事件早返回 identity — region 不关心这个原始事件 */
    if (evt == WAKE_EVT_NONE) {
        return out;
    }

    switch (*cur) {
    case WAKE_DORMANT:
        if (evt == WAKE_EVT_DETECT_SOURCE) {
            if (inp->wake_kind == WAKE_KIND_BTN) {
                /* 用户按右键。看时间。 */
                *cur = WAKE_FROM_BTN;
                out = add_action(out, ACT_DISPLAY_FADE_IN);
            } else if (inp->wake_kind == WAKE_KIND_RTC) {
                /* PCF85063 闹铃。强制 auto-play + 大音量 + 起 wifi。 */
                *cur = WAKE_FROM_RTC;
                out = add_action(out, ACT_DISPLAY_FADE_IN);
                out = add_action(out, ACT_VOLUME_MAX);
                out = add_action(out, ACT_NET_AUTO_CONNECT);
            } else {
                /* WAKE_KIND_NONE / WAKE_KIND_SYS: 冷启动,纯上电。 */
                *cur = WAKE_FROM_SYS;
            }
        }
        break;

    case WAKE_FROM_BTN:
        if (evt == WAKE_EVT_NO_CREDS) {
            /* 用户按键唤醒但发现没 WiFi 凭据,降级为配网流程 */
            *cur = WAKE_FROM_SYS;
        } else if (evt == WAKE_EVT_BTN_SLEEP_PRESS) {
            /* 用户按右键想睡。直接 deep sleep,不起 audio 不连 wifi */
            *cur = WAKE_GOTO_SLEEP;
            out = add_action(out, ACT_DISPLAY_OFF);
            out = add_action(out, ACT_DEEP_SLEEP);
        }
        /* 其他事件 (TICK_1HZ / DETECT_SOURCE 重复 等) → identity (stays) */
        break;

    case WAKE_FROM_RTC:
        if (evt == WAKE_EVT_NO_CREDS) {
            /* 闹铃触发但没 WiFi,降级为只显示时间 (audio_fsm 不会 auto-play) */
            *cur = WAKE_FROM_BTN;
        } else if (evt == WAKE_EVT_BOOT_DONE_FANOUT) {
            /* router 在 BOOT_DONE 后 fan-out 给 wake,通知可以放音频 */
            if (inp->weekend_skip) {
                /* 周末:跳闹铃,降级为只显示 */
                *cur = WAKE_FROM_BTN;
            } else {
                *cur = WAKE_ALARM_RINGING;
                out = add_action(out, ACT_AUDIO_AUTO_PLAY);
            }
        } else if (evt == WAKE_EVT_BTN_SLEEP_PRESS) {
            /* 闹铃刚醒来用户按右键想再睡 — 走 deep sleep */
            *cur = WAKE_GOTO_SLEEP;
            out = add_action(out, ACT_DISPLAY_OFF);
            out = add_action(out, ACT_DEEP_SLEEP);
        }
        /* TICK_1HZ → identity (等 BOOT_DONE_FANOUT) */
        break;

    case WAKE_ALARM_RINGING:
        if (evt == WAKE_EVT_BTN_LEFT_TOGGLE) {
            /* 用户按左键关掉闹钟,但设备继续运行 (退化为 FROM_BTN) */
            *cur = WAKE_FROM_BTN;
            out = add_action(out, ACT_AUDIO_STOP);
        } else if (evt == WAKE_EVT_ALARM_COMPLETE) {
            /* 闹钟时长到。arm 下次 + deep sleep */
            *cur = WAKE_GOTO_SLEEP;
            out = add_action(out, ACT_ARM_RTC_FOR_TOMORROW);
            out = add_action(out, ACT_DEEP_SLEEP);
        } else if (evt == WAKE_EVT_TICK_1HZ && inp->alarm_ring_minutes >= 15) {
            /* 15min 兜底:防止用户没按键导致永久响铃 */
            *cur = WAKE_GOTO_SLEEP;
            out = add_action(out, ACT_ARM_RTC_FOR_TOMORROW);
            out = add_action(out, ACT_DEEP_SLEEP);
        }
        /* 其他事件 → identity */
        break;

    case WAKE_FROM_SYS:
        /* 冷启动 / 配网阶段。wake 状态保持,sys/net/audio/display 各自处理 */
        /* 配网成功后由 NET_EVT_PROV_OK 等其他信号驱动后续 */
        break;

    case WAKE_GOTO_SLEEP:
        /* 终态前过渡。executor 看到 ACT_DEEP_SLEEP 立即 deep sleep。
         * 即便后续再有事件进来,这里也不动。 */
        break;
    }

    return out;
}