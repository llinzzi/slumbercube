/*
 * sys_fsm.c — 系统生命周期 FSM。
 *
 * 状态机精简:SYS_BOOT → SYS_NORMAL → SYS_SLEEPING (终态)
 *
 * sys 负责:
 *  - BOOT_DONE: 上电初始化完成后转 NORMAL
 *  - BTN_SLEEP_PRESS: 用户按右键,完整 deep sleep 流程
 *  - BTN_FACTORY_RESET: 三击,擦 NVS + 重启
 *  - PROV_OK: 配网成功,reboot (executor 决定)
 *  - PROV_FAIL: 配网失败,留在 NORMAL (clock-only mode)
 *  - DEEP_SLEEP_TICK: wake_fsm GOTO_SLEEP 后通知 sys 走 deep sleep
 *
 * wake_kind-specific 的 boot 行为 (BTN 不连网, RTC auto-play) 由 wake_fsm
 * 自己处理,sys 不重复。
 */

#include "sys_fsm.h"
#include <string.h>

static fsm_actions_t add_action(fsm_actions_t a, app_action_kind_t kind)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = kind;
        memset(&a.items[a.count].u, 0, sizeof(a.items[a.count].u));
        a.count++;
    }
    return a;
}

/* 标准 deep sleep 动作序列:清理硬件资源。
 * 注意:不包含 ACT_DEEP_SLEEP — 实际 deep sleep 由 main.c 的
 * fsm_sleep 路径统一处理(先配 GPIO wake mask,再睡)。 */
static fsm_actions_t emit_deep_sleep_actions(fsm_actions_t a)
{
    a = add_action(a, ACT_AUDIO_DEINIT);
    a = add_action(a, ACT_DISPLAY_OFF);
    a = add_action(a, ACT_GPIO_HOLD);
    a = add_action(a, ACT_TIMER_SET);
    return a;
}

fsm_actions_t sys_fsm_step(sys_state_t *cur, sys_evt_t evt, const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };
    (void)inp;

    if (evt == SYS_EVT_NONE) {
        return out;
    }

    switch (*cur) {
    case SYS_BOOT:
        if (evt == SYS_EVT_BOOT_DONE) {
            *cur = SYS_NORMAL;
            /* 无动作:net/audio/display/wake 各自处理 boot-time 转换 */
        } else if (evt == SYS_EVT_DEEP_SLEEP_TICK) {
            /* wake_fsm 在 BOOT 阶段就决定进 GOTO_SLEEP (罕见边缘情况,
             * 例如冷启动配网超时,用户在配网中按右键),sys 走 deep sleep */
            *cur = SYS_SLEEPING;
            out = emit_deep_sleep_actions(out);
        }
        break;

    case SYS_NORMAL:
        if (evt == SYS_EVT_BTN_SLEEP_PRESS) {
            *cur = SYS_SLEEPING;
            out = emit_deep_sleep_actions(out);
        } else if (evt == SYS_EVT_BTN_FACTORY_RESET) {
            *cur = SYS_SLEEPING;
            out = add_action(out, ACT_NVS_ERASE);  /* esp_restart, 不返回 */
        } else if (evt == SYS_EVT_PROV_OK) {
            /* 配网成功:交给 executor 调 esp_restart */
            *cur = SYS_SLEEPING;
        }
        /* SYS_EVT_PROV_FAIL → stays (clock-only mode, 不进 SLEEPING)
         * SYS_EVT_NET_OK   → stays (信息性,给 audio_fsm 用)
         * SYS_EVT_TICK_1HZ → stays (心跳) */
        break;

    case SYS_SLEEPING:
        /* 终态:任何事件都不动。executor 见到 ACT_DEEP_SLEEP 立即 deep sleep。 */
        break;
    }

    return out;
}