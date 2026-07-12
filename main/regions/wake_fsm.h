/*
 * wake_fsm.h — 唤醒来源与唤醒后生命周期 FSM
 *
 * 状态(6):
 *   WAKE_DORMANT        抽象上电前
 *   WAKE_FROM_BTN       用户按右键。Intent: 只显示,不连网不播放
 *   WAKE_FROM_RTC       闹铃。Intent: 强制 auto-play,最大音量
 *   WAKE_FROM_SYS       冷启动 / 配网
 *   WAKE_ALARM_RINGING  FROM_RTC 内的响铃子态
 *   WAKE_GOTO_SLEEP     终态前的过渡
 *
 * 见 plan 文档 "1. wake_fsm" 章节的完整转换矩阵。
 */
#ifndef WAKE_FSM_H
#define WAKE_FSM_H

#include "../app_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wake_state_e {
    WAKE_DORMANT = 0,
    WAKE_FROM_BTN,
    WAKE_FROM_RTC,
    WAKE_FROM_SYS,
    WAKE_ALARM_RINGING,
    WAKE_GOTO_SLEEP,
} wake_state_t;

typedef enum {
    WAKE_EVT_NONE = 0,
    WAKE_EVT_DETECT_SOURCE,        /* 一次性:boot 完成时由 router 发出 */
    WAKE_EVT_BTN_SLEEP_PRESS,       /* 右键短按 */
    WAKE_EVT_BTN_LEFT_TOGGLE,       /* 左键短按 (闹钟响起时关掉) */
    WAKE_EVT_ALARM_COMPLETE,        /* 15min timer 或用户主动 */
    WAKE_EVT_NO_CREDS,              /* 来自其他 region 的"无凭据"信号 */
    WAKE_EVT_BOOT_DONE_FANOUT,      /* router 在 BOOT_DONE 后追加的二次 fan-out */
    WAKE_EVT_TICK_1HZ,
} wake_evt_t;

/* 纯函数:主机可编译。evt == WAKE_EVT_NONE 时返回 {0 actions, 不修改 *cur}。 */
fsm_actions_t wake_fsm_step(wake_state_t *cur, wake_evt_t evt, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_FSM_H */
