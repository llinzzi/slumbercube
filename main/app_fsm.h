/*
 * app_fsm.h — 公共类型:5 个正交 region FSM 共享的事件 / 输入 / 动作类型。
 *
 * 设计原则:
 * - 每个 region 的状态 + 事件枚举在自己的 regions/<name>_fsm.h 里定义
 * - app_fsm.h 通过前向声明 typedef 把 region 状态类型嵌入 app_state_t
 * - region step 函数签名也在各自的 regions/<name>_fsm.h 中声明
 * - 此文件不依赖 ESP-IDF / LVGL / audio_player 等,可在主机上被直接 #include
 */

#ifndef APP_FSM_H
#define APP_FSM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── region 状态类型 ──────────────────────────────────────────────────────
 * 完整定义在 regions/<name>_fsm.h(通过下方 _state_t 字段使用它们)。
 * C 不允许 incomplete enum 作为 struct 字段,所以 app_state_t 实际使用 int
 * 字段;赋值时靠编译器 int↔enum 隐式转换。region step() 函数仍用强类型。 */

/* ── 唤醒来源 (boot 时由 main.c 一次性填入,后续由 wake_fsm 升级为状态) ── */
typedef enum {
    WAKE_KIND_NONE = 0,    /* 未检测 */
    WAKE_KIND_BTN,         /* 用户按右键 (CONFIG_WAKEUP_GPIO) */
    WAKE_KIND_RTC,         /* PCF85063 闹钟 (CONFIG_PCF85063_INT_GPIO) */
    WAKE_KIND_SYS,         /* 冷启动 / 异常 */
} wake_kind_t;

/* ── 音频播放器回调事件 (来自 esp-audio-player, 简化版,避免头依赖) ── */
typedef enum {
    APP_AUDIO_PLAYER_EVT_NONE = 0,
    APP_AUDIO_PLAYER_EVT_PLAYING,
    APP_AUDIO_PLAYER_EVT_IDLE,
    APP_AUDIO_PLAYER_EVT_NEXT,
    APP_AUDIO_PLAYER_EVT_PAUSE,
    APP_AUDIO_PLAYER_EVT_SHUTDOWN,
    APP_AUDIO_PLAYER_EVT_UNKNOWN_FILE,
    APP_AUDIO_PLAYER_EVT_ERROR,
} app_audio_player_evt_t;

/* ── 应用层动作种类 ──────────────────────────────────────────────────────── */
/* 一个动作最多带一个 payload(union)。动作执行顺序固定:
 *   wake → sys → net → audio → display
 * 每个 apply_actions() 内部:stop_* 在 init_* 之前,deinit 在 init 之前。 */
typedef enum {
    ACT_NONE = 0,
    /* 唤醒相关 */
    ACT_DISPLAY_FADE_IN,
    ACT_DISPLAY_FADE_OUT,
    ACT_VOLUME_MAX,                /* 闹钟唤醒强制最大音量 */
    ACT_VOLUME_RESTORE,            /* 闹钟完成后恢复 */
    ACT_ARM_RTC_FOR_TOMORROW,      /* arm 第二天同一时间的闹铃 */
    /* 显示 */
    ACT_DISPLAY_OFF,
    ACT_DISPLAY_BRIGHT,
    ACT_DISPLAY_STATION,           /* payload: const char * */
    ACT_DISPLAY_AUDIO_INDICATOR,   /* payload: bool on */
    ACT_DISPLAY_INDOOR_FULL,       /* payload: float temp_c, float humidity */
    ACT_DISPLAY_ALARM_TIME,        /* payload: int hour, int minute */
    ACT_DISPLAY_ALARM_OFF,
    ACT_DISPLAY_BUTTON_HINT,
    ACT_DISPLAY_BUTTON_HINT_AGENT_OFF,
    ACT_SET_NIGHT_MODE,            /* payload: bool on */
    ACT_SET_NIGHT_OVERRIDE,        /* payload: int8_t override */
    ACT_DRAW_MINIMAL_CLOCK,
    ACT_DRAW_WEATHER,
    /* 网络 */
    ACT_RUN_PROVISIONING,          /* 阻塞, 完成后回 PROV_OK/PROV_FAIL */
    ACT_WIFI_ENSURE_NETIF,
    ACT_WIFI_INIT_STA,
    ACT_WIFI_STA_ENSURE,
    ACT_WIFI_RECONNECT,
    ACT_NET_AUTO_CONNECT,          /* 闹钟唤醒专用的强制 wifi 起步 */
    ACT_NVS_ERASE_OLD_CREDS,       /* 自愈:擦掉错密码 */
    ACT_NTP_START,
    ACT_NTP_BLOCK_SYNC,            /* 阻塞 3s 等 SNTP 第一个响应 */
    /* 音频 */
    ACT_AUDIO_INIT,
    ACT_AUDIO_DEINIT,
    ACT_AUDIO_PLAY_URL,
    ACT_AUDIO_STOP,
    ACT_AUDIO_AUTO_PLAY,           /* 闹钟唤醒专用:跳过 agent 标志 */
    ACT_FETCH_API,                 /* audio_fetch_api() */
    /* 系统 */
    ACT_GPIO_HOLD,                 /* 持 pin 状态进入 deep sleep */
    ACT_TIMER_SET,                 /* esp_sleep_enable_timer_wakeup */
    ACT_DEEP_SLEEP,                /* esp_deep_sleep_start() */
    ACT_NVS_ERASE,                 /* factory reset */
    ACT_FACTORY_RESET,             /* NVS_ERASE + reboot */
    /* 维护 */
    ACT_LOG_HEAP,
    ACT_REFRESH_DISPLAY,           /* lvgl_adapter_refr_now */
    ACT_INDOOR_READ,               /* shtc3_read */
    ACT_SYNC_PCF_FROM_SYSTEM,      /* pcf85063_sync_from_system */
    ACT_APPLY_WEATHER,             /* screens_set_weather_data_ptr */
} app_action_kind_t;

/* ── 动作 + payload ──────────────────────────────────────────────────────── */
typedef struct {
    app_action_kind_t kind;
    union {
        struct { const char *name; }        station;
        struct { bool on; }                 indicator;
        struct { int hour, minute; }        alarm_time;
        struct { float temp_c, humidity; }  indoor;
        struct { int8_t override; }         night_override;
        struct { bool on; }                 night;
        struct { bool reconnect; }          audio_start;
    } u;
} fsm_action_t;

#define FSM_ACTIONS_MAX 8

typedef struct {
    fsm_action_t items[FSM_ACTIONS_MAX];
    uint8_t       count;
} fsm_actions_t;

/* ── 输入上下文 (executor 每 tick 组装后传给所有 region step) ──────────── */
typedef struct {
    /* 静态:boot 时一次性填入 */
    bool     has_creds;
    bool     agent_enabled;
    bool     weekend_skip;
    wake_kind_t wake_kind;          /* 仅 boot 第一次使用,后续由 wake 状态覆盖 */

    /* 动态:executor 每 tick 从各 region 状态 / 硬件读出 */
    bool     net_connected;
    bool     audio_url_set;
    bool     alarm_valid;
    bool     alarm_disabled;
    app_audio_player_evt_t last_audio_event;   /* 由 callback 线程原子写入 */
    uint32_t pending_ticks;
    uint32_t net_connect_ticks;    /* net_fsm CONNECTING 状态下的秒数 */
    uint8_t  stall_ticks;
    bool     first_advance_synced;
    uint8_t  alarm_ring_minutes;    /* 自 ALARM_RINGING 起累计分钟 */

    /* 当周是否周末 (Sat/Sun),仅 boot 一次 */
    bool     is_saturday;
    bool     is_sunday;
} app_input_t;

/* ── 复合应用状态:5 个 region 状态正交组合 ────────────────────────────────
 * 字段类型为 int(不是强 enum),因为 C 不允许 incomplete enum 作为结构体
 * 字段;region .h 中的强类型 enum 在赋值时通过隐式 int 转换互通。 */
typedef struct {
    int wake;       /* wake_state_t  : WAKE_DORMANT / FROM_BTN / FROM_RTC / FROM_SYS / ALARM_RINGING / GOTO_SLEEP */
    int sys;        /* sys_state_t   : SYS_BOOT / SYS_NORMAL / SYS_SLEEPING */
    int net;        /* net_state_t   : NET_OFFLINE / PROVISIONING / CONNECTING / CONNECTED / FAILED */
    int audio;      /* audio_state_t : AUDIO_IDLE / PENDING / INIT / PLAYING / STOPPING / ERROR */
    int display;    /* display_state_t : DISP_DAY / NIGHT_AUTO / NIGHT_FORCED */
} app_state_t;

/* ── 应用层原始事件 (由主循环 drain 后喂给 route_event) ───────────────── */
/* 注意:这些是 router 的输入,各 region 收到的子事件是它自己的 * 开头枚举。 */
typedef enum {
    EVT_NONE = 0,
    /* 唤醒检测 */
    EVT_BOOT_DONE,                    /* main.c 第一次进入主循环时发出 */
    EVT_WAKE_DETECT,                  /* 一次性,触发 wake_fsm 决定从哪种来源唤醒 */
    /* 定时 */
    EVT_TICK_1HZ,
    EVT_TICK_60S,
    /* 按钮(位图,可在同一 tick 内 fan-out) */
    EVT_BTN_SLEEP_PRESS,              /* 右键短按 */
    EVT_BTN_NIGHT_TOGGLE,             /* 右键长按 */
    EVT_BTN_PROVISION_REQUEST,        /* 右键三击 */
    EVT_BTN_AUDIO_TOGGLE,             /* 左键短按 */
    EVT_BTN_NTP_SYNC,                 /* 左键短按 (agent-off) */
    EVT_BTN_NEXT_TRACK,               /* 左键长按 */
    EVT_BTN_LEFT_TOGGLE,              /* 闹钟响铃时关掉 */
    /* WiFi */
    EVT_WIFI_STA_CONNECTED,
    EVT_WIFI_IP_GOT,
    EVT_WIFI_DISCONNECTED,
    EVT_WIFI_TIMEOUT,
    /* 配网 */
    EVT_PROVISION_OK,
    EVT_PROVISION_FAIL,
    /* 音频播放器回调 */
    EVT_AUDIO_PLAYER_PLAYING,
    EVT_AUDIO_PLAYER_IDLE,
    EVT_AUDIO_PLAYER_NEXT,
    EVT_AUDIO_PLAYER_ERROR,
    /* 唤醒完成 (ALARM_COMPLETE) */
    EVT_ALARM_COMPLETE,
    /* 系统 */
    EVT_DEEP_SLEEP_TICK,              /* wake_fsm 通告 sys_fsm 进入 deep sleep */
} app_event_t;

/* Convenience: 给 region step 返回的"该 region 不关心这个原始事件"语义。 */
#define REGION_EVT_NONE_FOR(cur, none_evt) (*(cur) == *(cur) ? (none_evt) : (none_evt))

#ifdef __cplusplus
}
#endif

#endif /* APP_FSM_H */
