/*
 * audio_fsm.h — 音频播放 FSM
 *
 * 状态(6): IDLE / PENDING / INIT / PLAYING / STOPPING / ERROR
 *
 * 见 plan 文档 "4. audio_fsm" 章节。
 */
#ifndef AUDIO_FSM_H
#define AUDIO_FSM_H

#include "../app_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum audio_state_e {
    AUDIO_IDLE = 0,
    AUDIO_PENDING,
    AUDIO_INIT,
    AUDIO_PLAYING,
    AUDIO_STOPPING,
    AUDIO_ERROR,
} audio_state_t;

typedef enum {
    AUDIO_EVT_NONE = 0,
    AUDIO_EVT_BTN_TOGGLE,
    AUDIO_EVT_BTN_NEXT,
    AUDIO_EVT_PLAYER_PLAYING,
    AUDIO_EVT_PLAYER_IDLE,
    AUDIO_EVT_PLAYER_NEXT,
    AUDIO_EVT_PLAYER_ERROR,
    AUDIO_EVT_TICK_1HZ,
    AUDIO_EVT_AGENT_DISABLED,
    AUDIO_EVT_AUTO_PLAY_REQUEST,     /* 来自 wake_fsm 闹钟唤醒 */
    AUDIO_EVT_ALARM_COMPLETE,        /* 闹钟结束后 wake_fsm 通知 */
    AUDIO_EVT_NET_OK_FANOUT,         /* router 在 IP_GOT 后给 audio 发 */
    AUDIO_EVT_STOP_DONE,             /* executor 调用 audio_stop() 后发出 */
} audio_evt_t;

fsm_actions_t audio_fsm_step(audio_state_t *cur, audio_evt_t evt, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_FSM_H */
