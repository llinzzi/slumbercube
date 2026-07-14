/*
 * sys_fsm.h — 系统生命周期 FSM
 *
 * 状态(3): SYS_BOOT -> SYS_NORMAL -> SYS_SLEEPING
 *
 * 见 plan 文档 "2. sys_fsm" 章节。
 */
#ifndef SYS_FSM_H
#define SYS_FSM_H

#include "../app_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sys_state_e {
    SYS_BOOT = 0,
    SYS_NORMAL,
    SYS_SLEEPING,
} sys_state_t;

typedef enum {
    SYS_EVT_NONE = 0,
    SYS_EVT_BOOT_DONE,
    SYS_EVT_BTN_SLEEP_PRESS,
    SYS_EVT_BTN_FACTORY_RESET,
    SYS_EVT_TICK_1HZ,
    SYS_EVT_NET_OK,
    SYS_EVT_PROV_OK,
    SYS_EVT_PROV_FAIL,
    SYS_EVT_DEEP_SLEEP_TICK,        /* 来自 wake_fsm GOTO_SLEEP 的推进 */
} sys_evt_t;

fsm_actions_t sys_fsm_step(sys_state_t *cur, sys_evt_t evt, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* SYS_FSM_H */
