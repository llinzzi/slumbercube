/*
 * display_fsm.c — 日/夜显示 FSM。
 *
 * 状态机:DAY <-> NIGHT_AUTO, NIGHT_FORCED 由用户强制覆盖
 *
 * TICK_1HZ 检测当前时间是否穿越 NIGHT_START_HOUR / NIGHT_END_HOUR
 * (executor 调用 clock_screen_is_night_time() 后把结果写到 inp->night_now)
 *
 * BTN_TOGGLE (右键长按) 在三态间循环:
 *   DAY          -> NIGHT_FORCED  (强制夜间)
 *   NIGHT_AUTO   -> NIGHT_FORCED  (强制夜间覆盖 auto)
 *   NIGHT_FORCED -> DAY (原 auto) / NIGHT_AUTO (原 auto,夜间时段)
 *                  决策依据:再次检查 inp->night_now
 */

#include "display_fsm.h"
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

static fsm_actions_t add_night_mode(fsm_actions_t a, bool on)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = ACT_SET_NIGHT_MODE;
        a.items[a.count].u.night.on = on;
        a.count++;
    }
    return a;
}

static fsm_actions_t add_night_override(fsm_actions_t a, int8_t override)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = ACT_SET_NIGHT_OVERRIDE;
        a.items[a.count].u.night_override.override = override;
        a.count++;
    }
    return a;
}

fsm_actions_t display_fsm_step(display_state_t *cur, display_evt_t evt, const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };

    if (evt == DISP_EVT_NONE) {
        return out;
    }

    switch (*cur) {
    case DISP_DAY:
        if (evt == DISP_EVT_TICK_1HZ && inp->night_now) {
            /* 时间穿越进入夜间时段 */
            *cur = DISP_NIGHT_AUTO;
            out = add_night_mode(out, true);
            out = add_action(out, ACT_DRAW_MINIMAL_CLOCK);
        } else if (evt == DISP_EVT_BTN_TOGGLE) {
            /* 用户长按右键:强制夜间 */
            *cur = DISP_NIGHT_FORCED;
            out = add_night_override(out, 1);  /* +1 = 强制夜间 */
            /* 注意:同时也要切到夜间显示。override 标志由 clock_screen
             * 自己读,我们只更新 override 值。 */
            out = add_night_mode(out, true);
        }
        /* AGENT_OFF → identity (informational) */
        break;

    case DISP_NIGHT_AUTO:
        if (evt == DISP_EVT_TICK_1HZ && !inp->night_now) {
            /* 时间穿越离开夜间时段 */
            *cur = DISP_DAY;
            out = add_night_mode(out, false);
            out = add_action(out, ACT_DRAW_WEATHER);
        } else if (evt == DISP_EVT_BTN_TOGGLE) {
            *cur = DISP_NIGHT_FORCED;
            out = add_night_override(out, 1);
            out = add_night_mode(out, true);
        }
        break;

    case DISP_NIGHT_FORCED:
        if (evt == DISP_EVT_BTN_TOGGLE) {
            /* 解除强制:回 DAY 或 NIGHT_AUTO 由当前时间决定 */
            if (inp->night_now) {
                *cur = DISP_NIGHT_AUTO;
                out = add_night_override(out, -1);  /* -1 = 自动 */
            } else {
                *cur = DISP_DAY;
                out = add_night_override(out, 0);  /* 0 = 强制白天 */
            }
            /* night_mode 关闭 (强制白天时) 或保持 (回 auto) */
            if (!inp->night_now) {
                out = add_night_mode(out, false);
            } else {
                out = add_night_mode(out, true);
            }
        }
        /* TICK_1HZ → identity (强制模式不响应时间穿越,等用户取消) */
        break;
    }

    return out;
}