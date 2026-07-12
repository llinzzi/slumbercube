/*
 * event_router.c — 骨架实现 (Step 1)。
 * 完整 fan-out 表在后续步骤按 plan 文档 "事件路由器" 章节补全。
 * 当前实现:所有输入都路由到 *NONE*,region step 全部 identity。
 */
#include "event_router.h"
#include <string.h>

routed_events_t route_event(app_event_t raw, const app_input_t *inp)
{
    (void)raw; (void)inp;
    routed_events_t r;
    memset(&r, 0, sizeof(r));
    /* 全部 *_EVT_NONE — region step 收到 NONE 时早返回 identity */
    return r;
}
