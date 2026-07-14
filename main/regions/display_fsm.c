/*
 * display_fsm.c — 日/夜显示 FSM。
 *
 * BTN_TOGGLE (右键长按) 简单 flip,匹配旧 right_long_press_cb 行为:
 *   任何状态 → 取反 inp->night_now → 强制白天或强制夜间
 * TICK_1HZ 自动检测时间穿越,从 DISP_DAY 进夜或从 DISP_NIGHT 出夜。
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

static fsm_actions_t add_night_override(fsm_actions_t a, int8_t o)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = ACT_SET_NIGHT_OVERRIDE;
        a.items[a.count].u.night_override.override = o;
        a.count++;
    }
    return a;
}

fsm_actions_t display_fsm_step(display_state_t *cur, display_evt_t evt,
                                const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };
    if (evt == DISP_EVT_NONE) return out;

    switch (*cur) {
    case DISP_DAY:
        if (evt == DISP_EVT_TICK_1HZ && inp->night_now) {
            *cur = DISP_NIGHT_AUTO;
            out = add_night_mode(out, true);
            out = add_action(out, ACT_DRAW_MINIMAL_CLOCK);
        } else if (evt == DISP_EVT_BTN_TOGGLE) {
            /* flip 到对面:白天 → 强制夜间 */
            *cur = DISP_NIGHT_FORCED;
            out = add_night_override(out, 1);
            out = add_night_mode(out, true);
        }
        break;

    case DISP_NIGHT_AUTO:
        if (evt == DISP_EVT_TICK_1HZ && !inp->night_now) {
            *cur = DISP_DAY;
            out = add_night_mode(out, false);
            out = add_action(out, ACT_DRAW_WEATHER);
        } else if (evt == DISP_EVT_BTN_TOGGLE) {
            /* flip 到对面:夜间 → 强制白天 */
            *cur = DISP_DAY;
            out = add_night_override(out, 0);  /* 0 = 强制白天 */
            out = add_night_mode(out, false);
        }
        break;

    case DISP_NIGHT_FORCED:
        if (evt == DISP_EVT_BTN_TOGGLE) {
            /* flip 到对面:强制夜间 → 白天(不回到 auto) */
            *cur = DISP_DAY;
            out = add_night_override(out, 0);
            out = add_night_mode(out, false);
        }
        /* TICK_1HZ → identity (强制模式不响应时间穿越) */
        break;
    }

    return out;
}