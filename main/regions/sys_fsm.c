/*
 * sys_fsm.c — 骨架实现 (Step 1)。
 * 完整转换在后续步骤按 plan 文档 "2. sys_fsm" 章节补全。
 */
#include "sys_fsm.h"
#include <string.h>

fsm_actions_t sys_fsm_step(sys_state_t *cur, sys_evt_t evt, const app_input_t *inp)
{
    (void)cur; (void)evt; (void)inp;
    fsm_actions_t out;
    memset(&out, 0, sizeof(out));
    return out;
}
