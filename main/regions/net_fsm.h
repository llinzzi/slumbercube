/*
 * net_fsm.h — 网络状态 FSM
 *
 * 状态(5): OFFLINE / PROVISIONING / CONNECTING / CONNECTED / FAILED
 *
 * 见 plan 文档 "3. net_fsm" 章节。
 */
#ifndef NET_FSM_H
#define NET_FSM_H

#include "../app_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum net_state_e {
    NET_OFFLINE = 0,
    NET_PROVISIONING,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_FAILED,
} net_state_t;

typedef enum {
    NET_EVT_NONE = 0,
    NET_EVT_NO_CREDS_AT_BOOT,
    NET_EVT_BTN_REQUEST_PROVISION,
    NET_EVT_PROV_OK,
    NET_EVT_PROV_FAIL,
    NET_EVT_STA_CONNECTED,
    NET_EVT_IP_GOT,
    NET_EVT_STA_DISCONNECTED,
    NET_EVT_WIFI_RETRY_EXHAUSTED,
    NET_EVT_TICK_1HZ,
} net_evt_t;

fsm_actions_t net_fsm_step(net_state_t *cur, net_evt_t evt, const app_input_t *inp);

#ifdef __cplusplus
}
#endif

#endif /* NET_FSM_H */
