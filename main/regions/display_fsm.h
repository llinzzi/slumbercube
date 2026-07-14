/*
 * display_fsm.h — 日/夜显示 FSM
 *
 * 状态(3): DAY / NIGHT_AUTO / NIGHT_FORCED
 *
 * 见 plan 文档 "5. display_fsm" 章节。
 */
#ifndef DISPLAY_FSM_H
#define DISPLAY_FSM_H

#include "../app_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum display_state_e {
    DISP_DAY = 0,
    DISP_NIGHT_AUTO,
    DISP_NIGHT_FORCED,
} display_state_t;

typedef enum {
    DISP_EVT_NONE = 0,
    DISP_EVT_TICK_1HZ,
    DISP_EVT_BTN_TOGGLE,
    DISP_EVT_AGENT_OFF,
} display_evt_t;

fsm_actions_t display_fsm_step(display_state_t *cur, display_evt_t evt, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_FSM_H */
