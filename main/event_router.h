/*
 * event_router.h — 把原始 app_event_t 分发到 5 个 region 的子事件。
 *
 * 大多数事件 1-1(例如 EVT_BTN_SLEEP_PRESS → 仅 wake 子事件非 NONE)。
 * 少数 fan-out:EVT_TICK_1HZ 转到 5 个 region 全部;
 *              EVT_BOOT_DONE 视 s_state.wake 决定是否给 audio 发 AUTO_PLAY_REQUEST;
 *              EVT_WIFI_IP_GOT 给 net + audio 的 NET_OK_FANOUT;
 *              EVT_ALARM_COMPLETE 转到 wake + audio。
 *
 * 当 region 不关心时,对应输出字段是 <region>_EVT_NONE;region step 早返回 identity。
 */
#ifndef EVENT_ROUTER_H
#define EVENT_ROUTER_H

#include "app_fsm.h"
#include "regions/wake_fsm.h"
#include "regions/sys_fsm.h"
#include "regions/net_fsm.h"
#include "regions/audio_fsm.h"
#include "regions/display_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 子事件枚举的实际类型来自 regions/<name>_fsm.h。
     * 这里字段类型是 wake_evt_t / sys_evt_t 等(强类型),不是 int。 */
    wake_evt_t    wake;
    sys_evt_t     sys;
    net_evt_t     net;
    audio_evt_t   audio;
    display_evt_t display;
} routed_events_t;

/* 路由器主入口。纯函数,可主机测试。
 *  - raw:           主循环 drain 出的原始事件
 *  - inp:           由 executor 组装,含 s_state.wake 等影响 fan-out 的上下文
 *  - 返回:          各 region 应处理的子事件
 *
 * 始终返回一个 full struct;unused 子事件以 *_EVT_NONE 表示。 */
routed_events_t route_event(app_event_t raw, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_ROUTER_H */
