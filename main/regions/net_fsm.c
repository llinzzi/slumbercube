/*
 * net_fsm.c — 骨架实现 (Step 1)。
 * 完整转换在后续步骤按 plan 文档 "3. net_fsm" 章节补全。
 */
#include "net_fsm.h"
#include <string.h>

fsm_actions_t net_fsm_step(net_state_t *cur, net_evt_t evt, const app_input_t *inp)
{
    (void)cur; (void)evt; (void)inp;
    fsm_actions_t out;
    memset(&out, 0, sizeof(out));
    return out;
}
