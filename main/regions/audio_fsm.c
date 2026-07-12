/*
 * audio_fsm.c — 音频播放 FSM。
 *
 * 状态机:
 *   IDLE --(BTN/auto)--> INIT --(PLAYER_PLAYING)--> PLAYING
 *                                                    |
 *                                            +-----+-----+-----+
 *                                            v     v     v
 *                                         STOP    NEXT   IDLE (auto-advance or stall)
 *                                          |
 *                                          v
 *                                       IDLE (after STOP_DONE)
 *
 * 关键不变量 (测试矩阵断言):
 *  - PENDING 状态不发出 ACT_DISPLAY_AUDIO_INDICATOR(true)
 *    (修复 main.c:884 旧 bug:WiFi 还没连就提前亮 indicator)
 *  - IDLE + AUTO_PLAY_REQUEST 不受 agent_enabled 影响
 *    (闹钟唤醒的 auto-play 必须强制,用户没禁用 agent 也不能阻挡)
 *  - INIT + PLAYER_PLAYING 转 PLAYING 后,indicator 才允许亮
 */

#include "audio_fsm.h"
#include <string.h>

#define AUDIO_PENDING_TIMEOUT_SEC 30
#define STALL_THRESHOLD_TICKS     3

static fsm_actions_t add_action(fsm_actions_t a, app_action_kind_t kind)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = kind;
        memset(&a.items[a.count].u, 0, sizeof(a.items[a.count].u));
        a.count++;
    }
    return a;
}

static fsm_actions_t add_station(fsm_actions_t a, const char *name)
{
    if (a.count < FSM_ACTIONS_MAX) {
        a.items[a.count].kind = ACT_DISPLAY_STATION;
        a.items[a.count].u.station.name = name;
        a.count++;
    }
    return a;
}

/* 标准音频启动序列 (来自 audio_start_playback in main.c:219-272) */
static fsm_actions_t add_audio_start_seq(fsm_actions_t a)
{
    a = add_action(a, ACT_AUDIO_INIT);
    a = add_action(a, ACT_FETCH_API);
    a = add_action(a, ACT_AUDIO_PLAY_URL);
    a = add_action(a, ACT_ARM_RTC_FOR_TOMORROW);  /* 仅当 acfg->valid */
    return a;
}

/* 切曲/auto-advance 序列 (audio_stop + audio_deinit + 全套 init) */
static fsm_actions_t add_audio_reseq(fsm_actions_t a)
{
    a = add_action(a, ACT_AUDIO_STOP);
    a = add_action(a, ACT_AUDIO_DEINIT);
    a = add_audio_start_seq(a);
    return a;
}

fsm_actions_t audio_fsm_step(audio_state_t *cur, audio_evt_t evt, const app_input_t *inp)
{
    fsm_actions_t out = { .count = 0 };

    if (evt == AUDIO_EVT_NONE) {
        return out;
    }

    switch (*cur) {
    case AUDIO_IDLE:
        if (evt == AUDIO_EVT_BTN_TOGGLE) {
            if (!inp->agent_enabled) {
                /* agent 关:按钮忽略,不放电台 */
                /* identity */
            } else if (inp->net_connected) {
                *cur = AUDIO_INIT;
                out = add_audio_start_seq(out);
            } else {
                *cur = AUDIO_PENDING;
                /* 关键:不发出 ACT_DISPLAY_AUDIO_INDICATOR (修复旧 bug) */
                out = add_station(out, "Connecting...");
            }
        } else if (evt == AUDIO_EVT_AUTO_PLAY_REQUEST) {
            /* 闹钟唤醒强制 auto-play,忽略 agent 标志 */
            *cur = AUDIO_INIT;
            out = add_action(out, ACT_VOLUME_MAX);
            out = add_audio_start_seq(out);
        } else if (evt == AUDIO_EVT_AGENT_DISABLED) {
            /* identity — agent 关闭,音频保持 IDLE */
        }
        break;

    case AUDIO_PENDING:
        if (evt == AUDIO_EVT_NET_OK_FANOUT) {
            /* router 在 IP_GOT 后通知 audio */
            *cur = AUDIO_INIT;
            out = add_audio_start_seq(out);
        } else if (evt == AUDIO_EVT_TICK_1HZ &&
                   inp->pending_ticks >= AUDIO_PENDING_TIMEOUT_SEC) {
            *cur = AUDIO_ERROR;
            out = add_station(out, "WiFi failed");
        } else if (evt == AUDIO_EVT_BTN_TOGGLE) {
            /* 用户取消等待,回 IDLE */
            *cur = AUDIO_IDLE;
        }
        break;

    case AUDIO_INIT:
        if (evt == AUDIO_EVT_PLAYER_PLAYING) {
            *cur = AUDIO_PLAYING;
            /* 此时 indicator 才允许亮 (executor 检查状态再亮) */
        } else if (evt == AUDIO_EVT_PLAYER_ERROR) {
            *cur = AUDIO_ERROR;
            out = add_action(out, ACT_AUDIO_DEINIT);
            out = add_station(out, "audio_failure_station_name()");
        }
        /* TICK_1HZ / BTN / 其他 → identity (等待 PLAYER_PLAYING 或 ERROR) */
        break;

    case AUDIO_PLAYING:
        if (evt == AUDIO_EVT_BTN_TOGGLE) {
            *cur = AUDIO_STOPPING;
            out = add_action(out, ACT_AUDIO_STOP);
            out = add_action(out, ACT_DISPLAY_AUDIO_INDICATOR);
            /* indicator false payload */
            if (out.count > 0) {
                out.items[out.count - 1].u.indicator.on = false;
            }
        } else if (evt == AUDIO_EVT_BTN_NEXT) {
            /* 切曲 (同 state 重入,但走完整 deinit + init 序列) */
            out = add_audio_reseq(out);
            /* state 保持 PLAYING */
        } else if (evt == AUDIO_EVT_PLAYER_IDLE) {
            /* 自然播完:进入下一首 (auto-advance) */
            out = add_audio_reseq(out);
        } else if (evt == AUDIO_EVT_PLAYER_NEXT) {
            /* audio_player 主动通知换曲 */
            out = add_audio_reseq(out);
        } else if (evt == AUDIO_EVT_PLAYER_ERROR) {
            *cur = AUDIO_ERROR;
            out = add_action(out, ACT_AUDIO_DEINIT);
            out = add_station(out, "audio_failure_station_name()");
        } else if (evt == AUDIO_EVT_ALARM_COMPLETE) {
            /* 闹钟时长到:立即关电台 (但 device 继续 wake → GOTO_SLEEP 由 wake_fsm 决定) */
            *cur = AUDIO_STOPPING;
            out = add_action(out, ACT_AUDIO_STOP);
        } else if (evt == AUDIO_EVT_TICK_1HZ &&
                   inp->stall_ticks >= STALL_THRESHOLD_TICKS) {
            /* 3 秒 stall 兜底:强制 advance */
            out = add_audio_reseq(out);
        }
        /* 其他事件 → identity */
        break;

    case AUDIO_STOPPING:
        if (evt == AUDIO_EVT_STOP_DONE) {
            /* executor 完成 audio_stop() 后回 IDLE */
            *cur = AUDIO_IDLE;
        }
        /* 其他事件 → identity (audio_stop 还没完,等等) */
        break;

    case AUDIO_ERROR:
        if (evt == AUDIO_EVT_BTN_TOGGLE) {
            /* 用户按了重试 */
            *cur = AUDIO_INIT;
            out = add_audio_start_seq(out);
        }
        /* TICK_1HZ → identity (等用户干预) */
        break;
    }

    return out;
}