/*
 * net_fsm.c — 网络状态 FSM。
 *
 * 状态机:OFFLINE → (PROVISIONING | CONNECTING) ↔ CONNECTED,FAILED
 *
 * boot-time 决策(凭据在不在):
 *   BOOT_DONE & has_creds   -> CONNECTING + ACT_WIFI_INIT_STA
 *   BOOT_DONE & !has_creds  -> PROVISIONING + ACT_RUN_PROVISIONING
 *
 * Provisioning 完成:
 *   PROV_OK   -> OFFLINE (executor reboot 由 sys_fsm 走 DEEP_SLEEP)
 *   PROV_FAIL -> OFFLINE + ACT_DISPLAY_STATION("clock-only mode")
 *
 * 连接流程:
 *   CONNECTING + IP_GOT              -> CONNECTED + ACT_NTP_START
 *   CONNECTING + WIFI_RETRY_EXHAUSTED -> FAILED + ACT_NVS_ERASE_OLD_CREDS
 *   CONNECTING + TICK & ticks >= 30   -> FAILED
 *
 * 运行期:
 *   CONNECTED + STA_DISCONNECTED -> CONNECTING + ACT_WIFI_RECONNECT
 *   FAILED   + BTN_REQUEST_PROVISION -> PROVISIONING + ACT_RUN_PROVISIONING
 */

#include "net_fsm.h"
#include <string.h>

#define NET_CONNECT_TIMEOUT_SEC 30

static fsm_actions_t add_action(fsm_actions_t a, app_action_kind_t kind)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = kind;
        memset(&a.items[a.count].u, 0, sizeof(a.items[a.count].u));
        a.count++;
    }
    return a;
}

/* payload-bearing helper */
static fsm_actions_t add_station(fsm_actions_t a, const char *name)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = ACT_DISPLAY_STATION;
        a.items[a.count].u.station.name = name;
        a.count++;
    }
    return a;
}

fsm_actions_t net_fsm_step(net_state_t *cur, net_evt_t evt, const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };

    if (evt == NET_EVT_NONE) {
        return out;
    }

    switch (*cur) {
    case NET_OFFLINE:
        if (evt == NET_EVT_BOOT_DONE) {
            if (inp->has_creds) {
                *cur = NET_CONNECTING;
                out = add_action(out, ACT_WIFI_INIT_STA);
            } else {
                *cur = NET_PROVISIONING;
                out = add_action(out, ACT_RUN_PROVISIONING);
            }
        }
        break;

    case NET_PROVISIONING:
        if (evt == NET_EVT_PROV_OK) {
            /* 配网成功:creds 已写入 NVS。sys_fsm 会收到 SYS_EVT_PROV_OK 走
             * DEEP_SLEEP/reboot。这里 net_fsm 转回 OFFLINE,等下次启动
             * 再 BOOT_DONE → CONNECTING (有凭据了)。 */
            *cur = NET_OFFLINE;
        } else if (evt == NET_EVT_PROV_FAIL) {
            *cur = NET_OFFLINE;
            out = add_station(out, "clock-only mode");
        } else if (evt == NET_EVT_BTN_REQUEST_PROVISION) {
            /* 用户主动要求重新配网 (例如三击右键) */
            out = add_action(out, ACT_RUN_PROVISIONING);
            /* 状态保持 PROVISIONING */
        }
        break;

    case NET_CONNECTING:
        if (evt == NET_EVT_IP_GOT) {
            *cur = NET_CONNECTED;
            out = add_action(out, ACT_NTP_START);
        } else if (evt == NET_EVT_WIFI_RETRY_EXHAUSTED) {
            /* 10 次重试结束 (wifi.c MAX_RETRY)。自愈:擦 NVS 等用户重配 */
            *cur = NET_FAILED;
            out = add_action(out, ACT_NVS_ERASE_OLD_CREDS);
        } else if (evt == NET_EVT_TICK_1HZ &&
                   inp->net_connect_ticks >= NET_CONNECT_TIMEOUT_SEC) {
            /* 30s 兜底:NET_EVT_WIFI_RETRY_EXHAUSTED 没到(罕见)也强制 FAILED */
            *cur = NET_FAILED;
        }
        /* 其他事件 → identity */
        break;

    case NET_CONNECTED:
        if (evt == NET_EVT_STA_DISCONNECTED) {
            *cur = NET_CONNECTING;
            out = add_action(out, ACT_WIFI_RECONNECT);
        }
        break;

    case NET_FAILED:
        if (evt == NET_EVT_BTN_REQUEST_PROVISION) {
            *cur = NET_PROVISIONING;
            out = add_action(out, ACT_RUN_PROVISIONING);
        }
        /* TICK_1HZ → identity (等待用户干预) */
        break;
    }

    return out;
}