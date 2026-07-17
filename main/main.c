#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "lvgl.h"
#include "ui.h"
#include "wifi.h"
#include "wifi_provisioning.h"
#include "config_screen.h"
#include "agent_config.h"
#include "clock_screen.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include <time.h>
#include <math.h>
#include "iot_button.h"
#include "button_gpio.h"
#include "shtc3.h"

#if CONFIG_PCF85063_ENABLE
#include "pcf85063.h"
#include <sys/time.h>
#endif

#if CONFIG_I2C_SCAN_AT_BOOT
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#endif

#if CONFIG_AUDIO_ENABLE
#include "audio_player_wrapper.h"
#endif

static const char *TAG = "MAIN";

/* Log heap state for memory pressure diagnostics */
static void log_heap(const char *label)
{
    ESP_LOGI(TAG, "Heap[%s]: free=%" PRIu32 " min_free=%" PRIu32 " largest_block=%" PRIu32,
             label,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

/* 配置项通过 menuconfig 设置 (参见 Kconfig.projbuild) */

/* ── Button-to-main-task notifications (replaces volatile flags) ─────── */
#define EVENT_SLEEP_PENDING        (1 << 0)
#define EVENT_AUDIO_TOGGLE         (1 << 1)
#define EVENT_PROVISIONING_REQUEST (1 << 2)
#define EVENT_NEXT_TRACK           (1 << 3)
#define EVENT_NIGHT_TOGGLE         (1 << 4)
#define EVENT_NTP_SYNC             (1 << 5)

/* Forward declaration — the audio path uses sync_pcf85063_after_ntp() and
 * the implementation lives further down in this file. */
static void sync_pcf85063_after_ntp(uint32_t timeout_ms);

/* Forward declaration — the audio-start path also kicks off a background
 * NTP sync; full implementation lives further down. */
static void ntp_sync_task(void *arg);
#define EVENT_BUTTON_MASK          (EVENT_SLEEP_PENDING | EVENT_AUDIO_TOGGLE | \
                                    EVENT_PROVISIONING_REQUEST | EVENT_NEXT_TRACK | \
                                    EVENT_NIGHT_TOGGLE | EVENT_NTP_SYNC)

static TaskHandle_t s_main_task = NULL;  /* set at top of app_main() */

static button_handle_t g_btn_right = NULL;
static button_handle_t g_btn_left = NULL;

static volatile bool s_audio_playing = false;   /* read by button callbacks AND main loop */
static volatile bool s_in_provisioning = false; /* read by button callbacks during captive portal */
static bool s_audio_pending           = false; /* wifi connecting, start audio when done */
static int  s_audio_pending_ticks     = 0;     /* timeout counter for pending start */
static bool s_rtc_alarm_armed          = false; /* set after arm_pcf85063_alarm_wakeup() */
typedef enum { WAKE_BTN, WAKE_RTC, WAKE_SYS } wake_kind_t;
static wake_kind_t s_wake_kind = WAKE_SYS;  /* default: cold boot */
static bool s_normal_mode              = false; /* true only when we reached the
                                                        * post-provisioning "normal operation"
                                                        * page (NVS creds at boot, OR
                                                        * provisioning just submitted OK and
                                                        * we're about to reboot). Never set
                                                        * in clock-only / provisioning pages,
                                                        * so audio (I2S) and the SHTC3 I2C
                                                        * sensor stay completely uninitialised
                                                        * until the user actually gets WiFi
                                                        * working. */

/* Try the on-board SHTC3 sensor. Returns true and fills *temp_c on success.
 * Hardware variant without the sensor just returns false; display omits the value. */
static bool read_indoor_env(float *temp_c, float *humidity)
{
    float t = 0, h = 0;
    if (!shtc3_read(&t, &h)) return false;
    *temp_c = t;
    *humidity = h;
    ESP_LOGI(TAG, "SHTC3: indoor %.1f°C, %.0f%%RH", t, h);
    return true;
}

#if CONFIG_PCF85063_ENABLE
static void apply_pcf85063_time(void)
{
    pcf85063_datetime_t dt;
    if (pcf85063_read_datetime(&dt) != ESP_OK) return;
    if (dt.year < 2025 || dt.year > 2099) {
        ESP_LOGW(TAG, "PCF85063: implausible year %u, skipping", dt.year);
        return;
    }
    setenv("TZ", "UTC0", 1); tzset();
    struct tm tm = { .tm_year = dt.year - 1900, .tm_mon = dt.month - 1,
                     .tm_mday = dt.day, .tm_hour = dt.hour,
                     .tm_min = dt.minute, .tm_sec = dt.second, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    if (t == (time_t)-1) { setenv("TZ", "CST-8", 1); tzset(); return; }
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    setenv("TZ", "CST-8", 1); tzset();
    ESP_LOGI(TAG, "PCF85063: applied system time %04u-%02u-%02u %02u:%02u:%02u UTC",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

/* When the server explicitly disables the alarm (alarm.enabled=false),
 * srv->valid will also be false, so this returns false here. The arm path
 * short-circuits earlier, leaving no PCF85063 alarm to skip — the device
 * can only be woken by the button in that mode. */
static bool should_skip_alarm_today(void)
{
    const audio_alarm_config_t *srv = audio_get_alarm_config();
    if (!srv || !srv->valid) return false;
    time_t now = time(NULL);
    struct tm tm = {0}; localtime_r(&now, &tm);
    if (tm.tm_wday == 0 && !srv->weekend_sunday)   return true;
    if (tm.tm_wday == 6 && !srv->weekend_saturday) return true;
    return false;
}

static bool arm_pcf85063_alarm_wakeup(void)
{
    if (!pcf85063_is_present()) return false;

    /* Agent is disabled entirely — no /api/esp fetch, no alarm. Disable the
     * PCF85063 interrupt and clear any pending flag so a stale alarm from a
     * previous cycle doesn't leave the INT pin asserted. Same behaviour as
     * server-disabled: no alarm, button-only wake. */
    agent_config_t agent_cfg;
    if (agent_config_load(&agent_cfg) == ESP_OK && !agent_cfg.enabled) {
        ESP_LOGW(TAG, "PCF85063: agent disabled, disabling alarm");
        pcf85063_clear_alarm_flag();
        pcf85063_enable_alarm_int(false);
        return false;
    }

    /* Server explicitly disabled the alarm — honour it. Leave PCF85063
     * registers untouched (whatever they were set to last cycle) and do not
     * raise the IO0 wake pin. The caller will then also skip the internal
     * timer fallback, leaving only the button to wake the device. */
    const audio_alarm_config_t *srv = audio_get_alarm_config();
    if (srv && srv->disabled) {
        ESP_LOGW(TAG, "PCF85063: server disabled alarm, skipping arm");
        return false;
    }

    /* Alarm time comes exclusively from the server. If the API fetch
     * failed or the server didn't return a valid alarm, leave the
     * PCF85063 alarm unchanged — never fall back to Kconfig defaults. */
    if (!srv || !srv->valid) {
        ESP_LOGW(TAG, "PCF85063: no valid server alarm, not arming");
        return false;
    }
    /* Server alarm is in local time (CST, UTC+8). PCF85063 stores UTC
     * internally and compares alarm registers against its UTC clock. */
    uint8_t wake_h = (uint8_t)(((int)srv->hour + 24 - 8) % 24);
    uint8_t wake_m = srv->minute;
    ESP_LOGI(TAG, "PCF85063: server alarm %02d:%02d CST -> %02d:%02d UTC",
             srv->hour, srv->minute, wake_h, wake_m);
    pcf85063_alarm_t alarm = { .enable = true, .minute = wake_m, .hour = wake_h,
                                .day = PCF85063_ALARM_DISABLE,
                                .weekday = PCF85063_ALARM_DISABLE };
    esp_err_t err = pcf85063_set_alarm(&alarm);
    if (err != ESP_OK) { ESP_LOGW(TAG, "set_alarm failed"); return false; }
    err = pcf85063_enable_alarm_int(true);
    if (err != ESP_OK) { ESP_LOGW(TAG, "enable_alarm_int failed"); return false; }
    gpio_config_t int_cfg = { .pin_bit_mask = (1ULL << CONFIG_PCF85063_INT_GPIO),
                               .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
                               .pull_down_en = GPIO_PULLDOWN_DISABLE,
                               .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&int_cfg);

    /* Read back current PCF85063 time so the log shows both the alarm
     * target and the clock it's comparing against. */
    pcf85063_datetime_t now_dt;
    if (pcf85063_read_datetime(&now_dt) == ESP_OK) {
        ESP_LOGI(TAG, "PCF85063: current time %04u-%02u-%02u %02u:%02u:%02u UTC, "
                 "alarm armed for %02d:%02d UTC (IO%d wake)",
                 now_dt.year, now_dt.month, now_dt.day,
                 now_dt.hour, now_dt.minute, now_dt.second,
                 wake_h, wake_m, CONFIG_PCF85063_INT_GPIO);
    } else {
        ESP_LOGI(TAG, "PCF85063: alarm armed for %02d:%02d UTC (IO%d wake)",
                 wake_h, wake_m, CONFIG_PCF85063_INT_GPIO);
    }
    return true;
}
#endif

/* When audio_play_url() returns a non-OK code, decide which user-facing
 * message to show. The "Agent disabled" case is the most actionable — it
 * tells the user to re-enable the agent in the captive portal. */
static const char *audio_failure_station_name(void)
{
    agent_config_t cfg;
    if (agent_config_load(&cfg) == ESP_OK && !cfg.enabled) {
        return "Agent disabled";
    }
    return "WiFi failed";
}

static void apply_weather_and_indoor(const weather_data_t *w);

#if CONFIG_AUDIO_ENABLE
/* Start audio playback with optional WiFi reconnection and display updates.
 * On failure, updates the display with an error message and clears the
 * audio-playing flag. Returns ESP_OK on success. */
static esp_err_t audio_start_playback(bool reconnect_wifi)
{
    if (reconnect_wifi) {
        clock_screen_set_station_name("Connecting WiFi...");
        wifi_ensure_netif();
        if (wifi_init_sta() != ESP_OK && !wifi_is_connected()) {
            clock_screen_set_audio_indicator(false);
            clock_screen_set_station_name("WiFi failed");
            s_audio_playing = false;
            return ESP_FAIL;
        }
    }

    clock_screen_set_station_name("Starting audio...");
    if (audio_init() != ESP_OK) {
        clock_screen_set_audio_indicator(false);
        clock_screen_set_station_name("Audio init failed");
        s_audio_playing = false;
        return ESP_FAIL;
    }

    /* Kick off SNTP every time audio playback is about to start — the
     * audio pending / play path uses wifi_sta_ensure() which only connects
     * and does not start the SNTP client, so without this explicit call
     * pressing play-then-stop on a cold boot would skip NTP entirely. */
    wifi_ensure_netif();
    wifi_init_sta();

    /* audio_play_url() internally fetches /api/esp (weather + alarm + radio).
     * Apply weather + alarm AFTER the fetch (so they have fresh data) but
     * BEFORE checking the return value — so they display even if playback
     * cannot start (e.g. missing URL, transient network issue). */
    esp_err_t play_rc = audio_play_url();

    apply_weather_and_indoor(audio_get_weather());

    {
        const audio_alarm_config_t *acfg = audio_get_alarm_config();
        if (acfg && acfg->valid) {
            clock_screen_set_alarm_time(acfg->hour, acfg->minute);
#if CONFIG_PCF85063_ENABLE
            /* Do NOT spawn an NTP sync worker here — the audio-start path
             * already kicks off WiFi (and indirectly SNTP), but having two
             * workers racing to call wifi_init_sta / sntp_init_time from
             * different tasks triggered the sntp_init_time re-entry that
             * crashed earlier. Instead, the user's stop press (or any
             * later wake) will spawn the worker through the main-loop
             * NTP_SYNC handler. Sync from system → RTC is idempotent, so
             * waiting until the user-driven path is fine. */
            s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
        }
    }

    if (play_rc != ESP_OK) {
        clock_screen_set_audio_indicator(false);
        clock_screen_set_station_name(audio_failure_station_name());
        s_audio_playing = false;
        return ESP_FAIL;
    }

    clock_screen_set_station_name(audio_get_station_name());
    clock_screen_set_audio_indicator(true);
    s_audio_playing = true;
    return ESP_OK;
}
#endif

static void apply_weather_and_indoor(const weather_data_t *w)
{
    if (w && w->valid) {
        screens_set_weather_data_ptr(w);
    }
    float t = 0, h = 0;
    if (read_indoor_env(&t, &h)) {
        clock_screen_set_indoor_env(t, h);
        audio_set_indoor_env(t, h);
    }
}

/* ── 右键 (电源) callbacks ─────────────────────────────────────── */

/* 右键 short click: sleep immediately. Aborts captive portal if active. */
static void right_short_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "右键 short click → sleep");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    if (s_main_task) xTaskNotify(s_main_task, EVENT_SLEEP_PENDING, eSetBits);
}

/* 右键 long press: flip between night and day display.
 * No "auto" in the cycle — auto is only the default on wake. */
static void right_long_press_cb(void *button_handle, void *usr_data)
{
    bool currently_night = clock_screen_is_night_time();
    /* Force the opposite of what's currently shown */
    int8_t next = currently_night ? 0 : 1;  /* 0=day, 1=night */
    clock_screen_set_night_override(next);
    ESP_LOGI(TAG, "右键 long press → force %s", currently_night ? "DAY" : "NIGHT");
    if (s_main_task) xTaskNotify(s_main_task, EVENT_NIGHT_TOGGLE, eSetBits);
}

/* 右键 triple-click: factory reset + reboot into captive portal. */
static void right_triple_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "右键 triple click — factory reset");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    if (s_main_task) xTaskNotify(s_main_task, EVENT_PROVISIONING_REQUEST, eSetBits);
}

/* ── 左键 (媒体) callbacks ─────────────────────────────────────── */

/* Wait until SNTP has actually applied a server response to the system
 * clock, then sync PCF85063 from it. Used at every site that previously
 * called pcf85063_sync_from_system() unconditionally — that path was a
 * race: a fixed vTaskDelay(3000) doesn't guarantee SNTP got a response
 * in time, so we often ended up re-writing the RTC with the same
 * pre-sync value the boot already had. With the sync-notification flag
 * we wait up to timeout_ms for a real round-trip; if SNTP doesn't land
 * one in that window (slow DNS, captive portal, dropped UDP), we leave
 * the RTC alone and surface the timeout on screen instead of faking a
 * successful sync. */
static void sync_pcf85063_after_ntp(uint32_t timeout_ms)
{
#if CONFIG_PCF85063_ENABLE
    if (!pcf85063_is_present()) return;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!wifi_is_time_synced()) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "NTP sync did not complete within %u ms — "
                          "skipping RTC write to avoid persisting stale time",
                          timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (pcf85063_sync_from_system() == ESP_OK) {
        ESP_LOGI(TAG, "PCF85063 synced from NTP-corrected system time");
    } else {
        ESP_LOGW(TAG, "pcf85063_sync_from_system failed");
    }
#endif
}

/* Background NTP sync helper. Spawned as a one-shot task from the main loop
 * so the main loop is never blocked by the SNTP poll or RTC round-trip.
 * Self-deletes when finished. */
static void ntp_sync_task(void *arg)
{
    ESP_LOGI("NTP_SYNC", "worker started");
    if (!wifi_is_connected()) {
        wifi_ensure_netif();
        wifi_init_sta();
    }

    sync_pcf85063_after_ntp(30000);

    if (wifi_is_time_synced()) {
        clock_screen_set_station_name("时间已更新");
    } else {
        clock_screen_set_station_name("时间同步超时,请稍候");
    }
    ESP_LOGI("NTP_SYNC", "worker done");
    vTaskDelete(NULL);
}

/* 左键 short click: agent enabled → toggle audio. The first time the user
 * STOPS playback in a wake cycle, also fire an NTP sync — that's the moment
 * they're physically at the clock with WiFi reachable, and a natural way to
 * catch the RTC up to network time without making them disable the agent.
 * Subsequent toggles are audio-only. Agent disabled → NTP sync on every press. */
static void left_short_click_cb(void *button_handle, void *usr_data)
{
#if CONFIG_AUDIO_ENABLE
    if (!s_normal_mode) {
        ESP_LOGI(TAG, "左键 short click — ignored (normal=%d)", s_normal_mode);
        return;
    }
    agent_config_t acfg;
    bool agent_on = (agent_config_load(&acfg) == ESP_OK && acfg.enabled);
    if (agent_on) {
        static bool s_ntp_synced_this_wake = false;
        uint32_t bits = EVENT_AUDIO_TOGGLE;
        /* `s_audio_playing` reflects whether the clock is currently producing
         * audio. If true, this click will be the stop transition — that's
         * where we want to opportunistically sync NTP, once per wake. */
        bool stopping = s_audio_playing;
        if (stopping && !s_ntp_synced_this_wake) {
            s_ntp_synced_this_wake = true;
            bits |= EVENT_NTP_SYNC;
            ESP_LOGI(TAG, "左键 short click → stop + NTP sync (first stop this wake)");
            clock_screen_set_station_name("同步时间...");
        } else {
            ESP_LOGI(TAG, "左键 short click → audio toggle (stopping=%d)",
                     stopping);
        }
        if (s_main_task) xTaskNotify(s_main_task, bits, eSetBits);
        clock_screen_set_audio_indicator(!s_audio_playing);
    } else {
        ESP_LOGI(TAG, "左键 short click → NTP sync (agent off)");
        clock_screen_set_station_name("同步时间...");
        if (s_main_task) xTaskNotify(s_main_task, EVENT_NTP_SYNC, eSetBits);
    }
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring 左键 short click");
#endif
}

/* 左键 long press: skip to next track. Deinit → fetch /api/esp → play. */
static void left_long_press_cb(void *button_handle, void *usr_data)
{
#if CONFIG_AUDIO_ENABLE
    if (!s_normal_mode) {
        ESP_LOGI(TAG, "左键 long press — ignored (normal=%d)", s_normal_mode);
        return;
    }
    ESP_LOGI(TAG, "左键 long press → next track");
    if (s_main_task) xTaskNotify(s_main_task, EVENT_NEXT_TRACK, eSetBits);
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring 左键 long press");
#endif
}

/* Wipe NVS and reboot. Shared by the main-loop triple-click handler and the
 * boot-time provisioning path (so triple-click during the first-boot
 * captive portal takes effect right after the function returns, not after
 * a 5-min timeout). */
static void do_factory_reset(void)
{
    /* Clear notification bit (harmless if not set) */
    uint32_t bits;
    xTaskNotifyWait(0, EVENT_PROVISIONING_REQUEST, &bits, 0);
    ESP_LOGI(TAG, "Factory reset: erasing NVS and rebooting");

#if CONFIG_AUDIO_ENABLE
    if (s_audio_playing) {
        audio_stop();
        audio_deinit();
        s_audio_playing = false;
    }
#endif
    /* Blank the OLED so the post-reboot splash doesn't show partial state. */
    ssd1322_display_off();

    /* nvs_flash_erase clears every namespace — wifi_cfg, agent_cfg, clock. */
    esp_err_t er = nvs_flash_erase();
    ESP_LOGW(TAG, "nvs_flash_erase: %s", esp_err_to_name(er));

    /* Brief delay so the log line makes it to the UART before the reboot
     * tears the port down. */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void app_main(void)
{
    int64_t t_boot = esp_timer_get_time();
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", CONFIG_ACTIVE_DURATION_SECS);
    s_main_task = xTaskGetCurrentTaskHandle();

    /* Centralised NVS init — called once at boot so every other module
     * can rely on the partition being ready without re-initialising. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* Detect wake source: WAKE_BTN=右键, WAKE_RTC=alarm, WAKE_SYS=cold-boot */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_wake_kind = WAKE_SYS;
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        audio_set_wake_source("rtc");
        s_wake_kind = WAKE_RTC;
        ESP_LOGI(TAG, "Woke from RTC timer");
        break;
    case ESP_SLEEP_WAKEUP_GPIO: {
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        uint64_t wake_pins = esp_sleep_get_gpio_wakeup_status();
        /* Check button FIRST: if 右键 is in the wake mask, user pressed it. */
        if (wake_pins & (1ULL << CONFIG_WAKEUP_GPIO)) {
            audio_set_wake_source("btn");
            s_wake_kind = WAKE_BTN;
            ESP_LOGI(TAG, "Woke from 右键 (mask=0x%llX)", (unsigned long long)wake_pins);
            break;
        }
#if CONFIG_PCF85063_ENABLE
        if (wake_pins & (1ULL << CONFIG_PCF85063_INT_GPIO)) {
            audio_set_wake_source("rtc");
            s_wake_kind = WAKE_RTC;
            ESP_LOGI(TAG, "Woke from PCF85063 alarm (IO%d)", CONFIG_PCF85063_INT_GPIO);
            pcf85063_clear_alarm_flag();
            break;
        }
#endif
        /* Unknown GPIO — treat as button */
        audio_set_wake_source("btn");
        s_wake_kind = WAKE_BTN;
        ESP_LOGI(TAG, "Woke from unknown GPIO (mask=0x%llX)", (unsigned long long)wake_pins);
        break;
#else
        /* GPIO deep-sleep wakeup isn't available on this chip — nothing
         * to attribute the wake to. Treat as cold boot. */
        audio_set_wake_source("sys");
        s_wake_kind = WAKE_SYS;
        ESP_LOGW(TAG, "GPIO wakeup reported but not supported on this chip");
        break;
#endif
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        audio_set_wake_source("sys");
        s_wake_kind = WAKE_SYS;
        ESP_LOGI(TAG, "Cold boot");
        break;
    default:
        audio_set_wake_source("sys");
        s_wake_kind = WAKE_SYS;
        ESP_LOGI(TAG, "Wake cause: %d (unhandled, using sys)", (int)cause);
        break;
    }

    /* Enable GPIO hold through deep sleep, and release any hold left from
     * previous sleep cycle before reconfiguring pins. */
    gpio_deep_sleep_hold_en();
    gpio_hold_dis(PIN_NUM_RST);
    gpio_hold_dis(CONFIG_PIN_NS4168_CTRL);
    gpio_hold_dis(CONFIG_WAKEUP_GPIO);
#if CONFIG_PCF85063_ENABLE
    gpio_hold_dis(CONFIG_PCF85063_INT_GPIO);
#endif

    /* Hold all control and SPI pins at known levels before SSD1322 init.
     * CS is hardwired to GND, so the SSD1322 SPI is always selected — any
     * floating or transitioning MOSI/SCLK during bootloader can be interpreted
     * as random commands and cause the display to light up with garbage. */
    gpio_config_t early_pins = {
        .pin_bit_mask = (1ULL << PIN_NUM_RST) | (1ULL << PIN_NUM_DC) |
                        (1ULL << PIN_NUM_MOSI) | (1ULL << PIN_NUM_CLK) |
                        (1ULL << CONFIG_PIN_NS4168_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* RST pull-down ensures SSD1322 stays in reset if GPIO floats during bootloader */
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&early_pins);
    gpio_set_level(PIN_NUM_RST, 0);        /* Keep SSD1322 in reset */
    gpio_set_level(PIN_NUM_MOSI, 0);       /* MOSI low */
    gpio_set_level(PIN_NUM_CLK, 0);        /* SCLK low */
    gpio_set_level(PIN_NUM_DC, 0);         /* DC low */
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0); /* NS4168 shutdown */
    int64_t t1 = esp_timer_get_time();

    // Initialize SSD1322 driver first (display stays OFF until first frame rendered)
    ESP_ERROR_CHECK(ssd1322_init());
    int64_t t2 = esp_timer_get_time();

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

#if CONFIG_I2C_SCAN_AT_BOOT
    /* Debug-only: probe the shared I²C bus from 0x00..0x7F so we can see
     * which addresses actually ACK before any device-specific probe runs.
     * Config-gated; production builds leave it off (Kconfig default n). */
    i2c_bus_init();
    i2c_master_bus_handle_t scan_bus = i2c_bus_get();
    ESP_LOGW("I2C_SCAN", "scanning 0x00..0x7F on bus=%p", (void *)scan_bus);
    uint8_t scan_found = 0;
    for (uint8_t addr = 0; addr < 0x80; addr++) {
        if (i2c_master_probe(scan_bus, addr, 50) == ESP_OK) {
            ESP_LOGW("I2C_SCAN", "  ACK at 0x%02X", addr);
            scan_found++;
        }
    }
    ESP_LOGW("I2C_SCAN", "done — %u device(s) ACK'd", scan_found);
#endif

#if CONFIG_PCF85063_ENABLE
    pcf85063_init();
    apply_pcf85063_time();
#endif
    int64_t t3 = esp_timer_get_time();

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());
    int64_t t4 = esp_timer_get_time();
    log_heap("lvgl_init");

    // Wait for LVGL task to start (one tick at 100Hz = 10ms; LVGL task prio 3
    // gets scheduled within the first tick, so one tick is sufficient)
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Route to one of two pages based on NVS state. No more init-both-then-swap:
     * clock and QR are independent LVGL screens, each loaded only on its own path. */
    wifi_creds_t boot_creds;
    bool has_creds = (wifi_creds_load(&boot_creds) == ESP_OK);
    s_normal_mode = has_creds;  /* unless provisioning later turns this off */

    if (has_creds) {
        ESP_ERROR_CHECK(ui_wrapper_init());
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char ap_ssid[32];
        snprintf(ap_ssid, sizeof(ap_ssid), "SlumberCube-%02X%02X", mac[4], mac[5]);
        config_screen_init(ap_ssid, CONFIG_WIFI_PROV_AP_PASSWORD);
        config_screen_show();
    }
    int64_t t5 = esp_timer_get_time();

    /* Force a synchronous flush so the active screen is in GDDRAM *before*
     * we turn the panel on. The lvgl_task is running at 10 ms intervals and
     * may have queued a flush of the default (now black) screen; the sync
     * flush here runs immediately and overwrites GDDRAM with the real screen.
     * This is the second half of the anti-white-flash fix (the first half
     * is painting the default screen black in lvgl_adapter_init). */
    lv_refr_now(lv_disp_get_default());
    int64_t t6 = esp_timer_get_time();

    // Turn on display AFTER first frame is in GDDRAM — eliminates white flash on wake
    ssd1322_display_on();
    int64_t t7 = esp_timer_get_time();

    // Brief settle before sensor I2C (display already rendered)
    vTaskDelay(pdMS_TO_TICKS(10));
    int64_t t8 = esp_timer_get_time();

    // ── 右键 (电源) (wake / sleep / night toggle / factory reset) ──
    {
        button_config_t btn_cfg = {
            .short_press_time = 300,
            .long_press_time = 2000,  /* 2s hold, fires on threshold before release */
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = CONFIG_RIGHT_BUTTON_GPIO,
            .active_level = 0,
            .enable_power_save = false,
        };
        esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn_right);
        if (err == ESP_OK) {
            iot_button_register_cb(g_btn_right, BUTTON_SINGLE_CLICK, NULL,
                                   right_short_click_cb, NULL);
            iot_button_register_cb(g_btn_right, BUTTON_LONG_PRESS_START, NULL,
                                   right_long_press_cb, NULL);
            button_event_args_t triple_args = { .multiple_clicks.clicks = 3 };
            iot_button_register_cb(g_btn_right, BUTTON_MULTIPLE_CLICK, &triple_args,
                                   right_triple_click_cb, NULL);
            ESP_LOGI(TAG, "右键 on IO%d (short=sleep, long=night, triple=reset)",
                     CONFIG_RIGHT_BUTTON_GPIO);
        } else {
            ESP_LOGE(TAG, "右键 init failed on IO%d: %s",
                     CONFIG_RIGHT_BUTTON_GPIO, esp_err_to_name(err));
        }
    }

    // ── 左键 (媒体) (play/pause / next track) ──
    {
        button_config_t btn_cfg = {
            .short_press_time = 300,
            .long_press_time = 800,
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = CONFIG_LEFT_BUTTON_GPIO,
            .active_level = 0,
            .enable_power_save = false,
        };
        esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn_left);
        if (err == ESP_OK) {
            iot_button_register_cb(g_btn_left, BUTTON_SINGLE_CLICK, NULL,
                                   left_short_click_cb, NULL);
            iot_button_register_cb(g_btn_left, BUTTON_LONG_PRESS_START, NULL,
                                   left_long_press_cb, NULL);
            ESP_LOGI(TAG, "左键 on IO%d (short=play/pause, long=next)",
                     CONFIG_LEFT_BUTTON_GPIO);
        } else {
            ESP_LOGE(TAG, "左键 init failed on IO%d: %s",
                     CONFIG_LEFT_BUTTON_GPIO, esp_err_to_name(err));
        }
    }

    int64_t t_post = esp_timer_get_time();

    /* ── Boot timing summary (microseconds from app_main entry) ── */
    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  BOOT TIMING (us from app_main)          ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ GPIO early init:   %6lld us            ║", (long long)(t1 - t_boot));
    ESP_LOGI(TAG, "║ ssd1322_init:      %6lld us            ║", (long long)(t2 - t1));
    ESP_LOGI(TAG, "║ timezone+RTC:      %6lld us            ║", (long long)(t3 - t2));
    ESP_LOGI(TAG, "║ lvgl_adapter_init: %6lld us            ║", (long long)(t4 - t3));
    ESP_LOGI(TAG, "║ LVGL wait+UI init: %6lld us            ║", (long long)(t5 - t4));
    ESP_LOGI(TAG, "║ lv_refr_now:       %6lld us            ║", (long long)(t6 - t5));
    ESP_LOGI(TAG, "║ display_on:        %6lld us            ║", (long long)(t7 - t6));
    ESP_LOGI(TAG, "║ post-display wait: %6lld us            ║", (long long)(t8 - t7));
    ESP_LOGI(TAG, "║ button init:       %6lld us            ║", (long long)(t_post - t8));
    ESP_LOGI(TAG, "╠══════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ TOTAL to display:  %6lld us  (%lld ms) ║",
             (long long)(t7 - t_boot), (long long)((t7 - t_boot) / 1000));
    ESP_LOGI(TAG, "║ TOTAL to post-init:%6lld us  (%lld ms) ║",
             (long long)(t_post - t_boot), (long long)((t_post - t_boot) / 1000));
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");

    /* Read indoor sensor on every wake (for no-network display fallback).
     * The SHTC3 may need up to ~1 s to start ACKing after power-up — a
     * single read at +700 ms reliably fails on cold boot, leaving the
     * no-network weather_label stuck at "". Retry once after a short
     * delay so the value lands on screen without waiting for the 60 s
     * poll. The 800 ms cost is negligible vs the 1800 s active window. */
    float s_indoor_t = NAN, s_indoor_h = NAN;
    read_indoor_env(&s_indoor_t, &s_indoor_h);
    if (isnan(s_indoor_t)) {
        vTaskDelay(pdMS_TO_TICKS(800));
        read_indoor_env(&s_indoor_t, &s_indoor_h);
        if (!isnan(s_indoor_t)) {
            ESP_LOGI(TAG, "Indoor sensor retry succeeded after warm-up delay");
        }
    }

    if (!clock_screen_is_night_time()) {
        /* First-boot provisioning: if NVS has no creds (and the user hasn't
         * disabled auto-provisioning in menuconfig), run the SoftAP captive
         * portal BEFORE attempting STA. This blocks until the user submits
         * creds OR times out. */
#if CONFIG_WIFI_PROV_AUTO_ON_FIRST_BOOT
        if (!has_creds) {
            ESP_LOGW(TAG, "No NVS creds, entering provisioning");
            wifi_ensure_netif();
            s_in_provisioning = true;
            wifi_prov_result_t pr = wifi_provisioning_run();
            s_in_provisioning = false;
            if (pr == WIFI_PROV_OK) {
                /* Creds are now in NVS. Reboot so the device starts clean
                 * (driver init, SNTP, audio) on the freshly-saved WiFi. */
                ESP_LOGI(TAG, "Credentials saved — rebooting into normal mode");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            /* Triple-click during the boot-time portal aborts provisioning
             * and notifies us. Honour it now — before any wifi/audio init —
             * so the user doesn't sit through 30s of retries. */
            uint32_t peek_bits = 0;
            xTaskNotifyWait(0, EVENT_PROVISIONING_REQUEST, &peek_bits, 0);
            if (peek_bits & EVENT_PROVISIONING_REQUEST) {
                do_factory_reset();
            }
            ESP_LOGW(TAG, "Provisioning did not save creds, clock-only mode (no WiFi)");
            s_normal_mode = false;  /* no creds → no audio, no SHTC3 */
        }
#endif

        /* ── Networking: only RTC alarm goes online. ────────────── */

        if (s_wake_kind != WAKE_RTC) {
            /* ── No network: show indoor env; alarm only when agent enabled ── */
            ESP_LOGI(TAG, "No-network display (wake=%d)", s_wake_kind);
            clock_screen_set_indoor_full(s_indoor_t, s_indoor_h);
            {
                agent_config_t acfg_alarm;
                bool agent_on = (agent_config_load(&acfg_alarm) == ESP_OK && acfg_alarm.enabled);
                if (agent_on) {
#if CONFIG_PCF85063_ENABLE
                    if (pcf85063_is_present()) {
                        pcf85063_alarm_t al;
                        if (pcf85063_read_alarm(&al) == ESP_OK && al.enable
                            && al.hour != PCF85063_ALARM_DISABLE
                            && al.minute != PCF85063_ALARM_DISABLE) {
                            /* PCF85063 stores alarm in UTC; convert to CST for display */
                            int display_h = ((int)al.hour + 8) % 24;
                            clock_screen_set_alarm_time(display_h, al.minute);
                        } else {
                            /* Agent on, chip present, but RTC alarm is either
                             * disabled, read failed, or sentinel field set.
                             * Show "闹铃关闭" so the user can distinguish
                             * "no alarm configured" from "no network". We
                             * still don't fall back to CONFIG defaults —
                             * the alarm text always comes from the server,
                             * never from menuconfig. */
                            clock_screen_set_alarm_off();
                        }
                    } else {
                        /* Agent on, but PCF85063 absent on I2C bus. */
                        clock_screen_set_alarm_off();
                    }
#else
                    /* Agent on, PCF85063 driver disabled in sdkconfig. */
                    clock_screen_set_alarm_off();
#endif
                }
                if (agent_on) {
                    clock_screen_show_button_hint();
                } else {
                    clock_screen_show_button_hint_agent_off();
                }
            }
        } else {
            /* ── RTC alarm wake: full network + weather + auto-play ── */
            clock_screen_set_station_name("Connecting WiFi...");
            wifi_ensure_netif();
            if (wifi_init_sta() == ESP_OK) {
                if (!wifi_is_time_set()) {
                    wifi_mark_time_set();
                }
#if CONFIG_PCF85063_ENABLE
                /* Spawn NTP sync in the background — see the matching
                 * comment in audio_start_playback(). The RTC alarm-wake
                 * path should not block the main loop on a 30 s SNTP
                 * round-trip — audio / button handling stays responsive. */
                xTaskCreate(ntp_sync_task, "ntp_sync_w", 3072, NULL, 1, NULL);
#endif
                log_heap("wifi_connected");
            }
        }
    } else {
        /* Night mode — but if no creds are saved, still need provisioning,
         * otherwise the user can never get WiFi set up. */
#if CONFIG_WIFI_PROV_AUTO_ON_FIRST_BOOT
        if (!has_creds) {
            ESP_LOGW(TAG, "No NVS creds (night mode), entering provisioning");
            wifi_ensure_netif();
            s_in_provisioning = true;
            wifi_prov_result_t pr = wifi_provisioning_run();
            s_in_provisioning = false;
            if (pr == WIFI_PROV_OK) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            uint32_t peek_bits2 = 0;
            xTaskNotifyWait(0, EVENT_PROVISIONING_REQUEST, &peek_bits2, 0);
            if (peek_bits2 & EVENT_PROVISIONING_REQUEST) {
                do_factory_reset();
            }
        } else {
            ESP_LOGI(TAG, "Night mode, display only");
        }
#else
        ESP_LOGI(TAG, "Night mode, display only");
#endif
    }

#if CONFIG_AUDIO_ENABLE
    ESP_LOGI(TAG, "Audio block guard: wake=%d normal=%d night=%d",
             s_wake_kind, s_normal_mode, clock_screen_is_night_time());
    if (s_wake_kind == WAKE_RTC && s_normal_mode && !clock_screen_is_night_time()) {
#if CONFIG_PCF85063_ENABLE
        if (should_skip_alarm_today()) {
            ESP_LOGI(TAG, "Weekend — alarm suppressed, clock-only wake");
            clock_screen_set_station_name("Weekend");
        } else
#endif
        {
        /* Read indoor sensor before any network call */
        float t = 0, h = 0;
        bool got_indoor = shtc3_read(&t, &h);
        if (got_indoor) {
            audio_set_indoor_env(t, h);
            ESP_LOGI(TAG, "SHTC3: indoor %.1f°C, %.0f%%RH", t, h);
        }

        if (audio_init() == ESP_OK) {
            log_heap("audio_init");
            clock_screen_set_station_name("Fetching weather...");
            esp_err_t fetch_rc = audio_fetch_api();
            bool got_weather = (fetch_rc == ESP_OK);

#if CONFIG_PCF85063_ENABLE
            s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
            /* Show alarm from server response (if valid). The no-network path
             * reads PCF85063 directly, but the WiFi path must also display it. */
            {
                const audio_alarm_config_t *acfg = audio_get_alarm_config();
                if (acfg && acfg->valid) {
                    clock_screen_set_alarm_time(acfg->hour, acfg->minute);
                }
            }
            if (fetch_rc == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGI(TAG, "Agent disabled by config, clock-only mode");
                clock_screen_set_station_name(audio_failure_station_name());
            }

            const weather_data_t *w = audio_get_weather();
            if (got_weather && w && w->valid) {
                screens_set_weather_data_ptr(w);
            }
            clock_screen_set_indoor_env(t, h);

            if (got_weather) {
                clock_screen_set_station_name(audio_get_station_name());
            }

            /* RTC alarm → auto-play. Cold boot → just fetch, no play. */
            if (s_wake_kind == WAKE_RTC && audio_radio_url_is_set()) {
                if (audio_play_url() == ESP_OK) {
                    clock_screen_set_station_name(audio_get_station_name());
                    clock_screen_set_audio_indicator(true);
                    s_audio_playing = true;
                }
            }
        }
        } /* !should_skip_alarm_today */
    }
#endif

    ESP_LOGI(TAG, "Running for %d seconds before sleep, button wakes", CONFIG_ACTIVE_DURATION_SECS);

    // ── Main loop: event-driven with 1s tick timeout ──
    for (uint32_t tick = 0; tick < (uint32_t)CONFIG_ACTIVE_DURATION_SECS; tick++) {
        uint32_t notified = 0;
        xTaskNotifyWait(0, EVENT_BUTTON_MASK, &notified, pdMS_TO_TICKS(1000));

        /* 右键 short click → sleep */
        if (notified & EVENT_SLEEP_PENDING) {
            break;
        }

        /* 右键 triple-click → factory reset */
        if (notified & EVENT_PROVISIONING_REQUEST) {
            do_factory_reset();
        }

        /* IO3 long press → apply night mode switch (deferred from callback) */
        if (notified & EVENT_NIGHT_TOGGLE) {
            bool is_night = clock_screen_is_night_time();
            clock_screen_set_night_mode(is_night);
            ESP_LOGI(TAG, "Night mode applied: %d (override=%d)",
                     is_night, (int)clock_screen_get_night_override());
        }

#if CONFIG_AUDIO_ENABLE
        /* AUDIO TOGGLE BEFORE NTP: a stop-press must take effect immediately
         * — the user is standing at the clock and won't tolerate a 3s+
         * playback-tail as the NTP handler races for the bus. The NTP sync
         * still runs in the same tick, just after the audio state has
         * already flipped. */
        /* 左键 short click → toggle audio (stop = full stop, no resume).
         * When starting playback, first fetch /api/esp to update alarm & radio. */
        if (notified & EVENT_AUDIO_TOGGLE) {
            if (s_audio_playing) {
                audio_stop();
                clock_screen_set_audio_indicator(false);
                clock_screen_set_station_name("Stopped");
                s_audio_playing = false;
            } else if (wifi_is_connected()) {
                /* audio_start_playback() fetches /api/esp internally
                 * (via audio_play_url → audio_radio_fetch → audio_fetch_api)
                 * and applies alarm + weather automatically. */
                audio_start_playback(false);
            } else {
                wifi_ensure_netif();
                if (wifi_sta_ensure() == ESP_OK) {
                    clock_screen_set_station_name("Connecting WiFi...");
                    clock_screen_set_audio_indicator(true);
                    s_audio_pending = true;
                    s_audio_pending_ticks = 0;
                } else {
                    clock_screen_set_station_name("WiFi failed");
                    clock_screen_set_audio_indicator(false);
                }
            }
        }

        /* 左键 short click → NTP time sync. Spawning a one-shot background
         * task keeps the main loop free — the audio stop that accompanies
         * this same button press (agent-on first-stop path) can take a few
         * seconds to drain an HTTP audio stream, and stacking another 3 s
         * SNTP/SYNC delay on top of it tips us past the 5 s task-watchdog.
         * The main loop is not responsible for the SNTP poll at all; the
         * helper just kicks it off and exits. */
        if (notified & EVENT_NTP_SYNC) {
            xTaskCreate(ntp_sync_task, "ntp_sync", 3072, NULL, 1, NULL);
        }

        /* 左键 long press → next track (skip current, fetch new from /api/esp) */
        if (notified & EVENT_NEXT_TRACK) {
            ESP_LOGI(TAG, "Next track requested");
            if (s_audio_playing) {
                audio_stop();
            }
            audio_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));

            if (!wifi_is_connected()) {
                wifi_ensure_netif();
                wifi_init_sta();
            }

            clock_screen_set_station_name("Next song...");
            if (audio_init() == ESP_OK) {
                esp_err_t fetch_rc = audio_fetch_api();
                if (fetch_rc == ESP_OK) {
                    const weather_data_t *w = audio_get_weather();
                    float t = 0, h = 0;
                    bool got_indoor = read_indoor_env(&t, &h);
                    if (w && w->valid) {
                        screens_set_weather_data_ptr(w);
                    }
                    if (got_indoor) {
                        audio_set_indoor_env(t, h);
                        clock_screen_set_indoor_env(t, h);
                    }
#if CONFIG_PCF85063_ENABLE
                    s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
                    /* Update alarm display from server response. */
                    {
                        const audio_alarm_config_t *acfg = audio_get_alarm_config();
                        if (acfg && acfg->valid) {
                            clock_screen_set_alarm_time(acfg->hour, acfg->minute);
                        }
                    }
                }
                if (audio_radio_url_is_set()) {
                    if (audio_play_url() == ESP_OK) {
                        clock_screen_set_station_name(audio_get_station_name());
                        clock_screen_set_audio_indicator(true);
                        s_audio_playing = true;
                    }
                }
            }
        }

        /* Auto-advance: when a track finishes, fetch the next song from
         * /api/esp and continue playing in a loop. */
        if (s_audio_playing) {
            int progress = audio_get_progress();
            static int stall_ticks = 0;
            bool track_done = false;

            if (audio_is_finished()) {
                const char *name = audio_get_station_name();
                ESP_LOGI(TAG, "Track finished: '%s', advancing", name ? name : "unknown");
                track_done = true;
            } else if (progress >= 100) {
                stall_ticks++;
                if (stall_ticks >= 3) {
                    ESP_LOGW(TAG, "Decoder stalled, force-advancing");
                    track_done = true;
                }
            } else {
                stall_ticks = 0;
            }

            if (track_done) {
                static bool s_first_advance_synced = false;
                stall_ticks = 0;
                audio_deinit();
                ESP_LOGI(TAG, "Audio deinit, fetching next song...");
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* Sync PCF85063 from SNTP-corrected system time on the
                 * FIRST auto-advance only. SNTP runs in the background
                 * during playback; by the first track's end it should have
                 * a fresh NTP fix. Subsequent tracks reuse the same session. */
#if CONFIG_PCF85063_ENABLE
                /* Sync PCF85063 from SNTP-corrected system time on the
                 * FIRST auto-advance only — by then SNTP has had several
                 * seconds of polling, but wait for the actual notification
                 * to be sure. */
                if (!s_first_advance_synced) {
                    sync_pcf85063_after_ntp(30000);
                    s_first_advance_synced = true;
                }
#endif

                clock_screen_set_station_name("Next song...");
                if (!wifi_is_connected()) {
                    wifi_init_sta();
                }
                /* Fetch fresh API data for the next track */
                if (audio_init() == ESP_OK) {
                    esp_err_t fc = audio_fetch_api();
                    if (fc == ESP_OK) {
                        const weather_data_t *w = audio_get_weather();
                        float t2 = 0, h2 = 0;
                        bool gi = read_indoor_env(&t2, &h2);
                        if (w && w->valid) {
                            screens_set_weather_data_ptr(w);
                        }
                        if (gi) {
                            audio_set_indoor_env(t2, h2);
                            clock_screen_set_indoor_env(t2, h2);
                        }
#if CONFIG_PCF85063_ENABLE
                        s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
                        /* Update alarm display from server response. */
                        const audio_alarm_config_t *acfg2 = audio_get_alarm_config();
                        if (acfg2 && acfg2->valid) {
                            clock_screen_set_alarm_time(acfg2->hour, acfg2->minute);
                        }
                    }
                    if (audio_radio_url_is_set()) {
                        if (audio_play_url() == ESP_OK) {
                            clock_screen_set_station_name(audio_get_station_name());
                            clock_screen_set_audio_indicator(true);
                            s_audio_playing = true;
                        }
                    }
                }
            }
        }

#endif

        /* Refresh indoor temp every 60s — room T/RH time constant is
         * minutes, no perceptual benefit from faster updates; combined
         * with the SHTC3 SLEEP-between-reads change this drops average
         * sensor current to ~0.13 µA. */
        if (tick % 60 == 0) {
            float t = 0, h = 0;
            if (shtc3_read(&t, &h)) {
                audio_set_indoor_env(t, h);
                clock_screen_set_indoor_env(t, h);
            }
        }

        /* Periodic heap check every 60s */
        if (tick % 60 == 0) {
            log_heap("active_loop");
        }

        /* Pending audio start — WiFi was background-started, poll for IP */
        if (s_audio_pending) {
            if (wifi_is_connected()) {
                s_audio_pending = false;
                /* audio_start_playback() fetches /api/esp internally
                 * and applies alarm + weather automatically. */
                audio_start_playback(false);
            } else if (++s_audio_pending_ticks >= 30) {
                s_audio_pending = false;
                clock_screen_set_audio_indicator(false);
                clock_screen_set_station_name("WiFi failed");
            }
        }

        /* Full-screen refresh every second */
        lvgl_adapter_refr_now();
    }

    ESP_LOGI(TAG, "Time to sleep, turning off display");
    log_heap("pre_sleep");

    /* Stop audio FIRST so the mixer (prio 5) and HTTP download (prio 6)
     * tasks are fully shut down before we delete the I2S channel and
     * power down the amp. This avoids a race where the mixer task
     * could write to a deleted I2S channel during audio_deinit(). */
#if CONFIG_AUDIO_ENABLE
    audio_stop();
    audio_deinit();
#endif

    /* Free canvas buffer (16KB) — safe, canvas is not drawn again */
    clock_screen_deinit();

    /* Kill display and amp after audio tasks are stopped */
    ssd1322_display_off();
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);

    // Drive RST low and hold through deep sleep to prevent SSD1322 from
    // exiting reset during wake transition (which causes white flash)
    gpio_set_level(PIN_NUM_RST, 0);
    gpio_hold_en(PIN_NUM_RST);

    // Hold NS4168 CTRL low through deep sleep to keep audio amp off
    gpio_hold_en(CONFIG_PIN_NS4168_CTRL);

    /* Enable internal pull-up on wakeup GPIO for reliable deep-sleep wake */
    gpio_set_pull_mode(CONFIG_WAKEUP_GPIO, GPIO_PULLUP_ONLY);
    gpio_hold_en(CONFIG_WAKEUP_GPIO);

    uint64_t wake_mask = (1ULL << CONFIG_WAKEUP_GPIO);

#if CONFIG_PCF85063_ENABLE
    if (s_rtc_alarm_armed) {
        wake_mask |= (1ULL << CONFIG_PCF85063_INT_GPIO);
        gpio_hold_en(CONFIG_PCF85063_INT_GPIO);
    }
#endif

#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    esp_deep_sleep_enable_gpio_wakeup(wake_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    ESP_LOGI(TAG, "GPIO wake mask: 0x%llX", (unsigned long long)wake_mask);
#else
    ESP_LOGW(TAG, "GPIO deep sleep wakeup not supported on this chip, wake by timer only");
#endif

#if CONFIG_PCF85063_ENABLE
    /* Fall back to internal RTC timer only when PCF85063 is unavailable;
     * a server-disabled alarm must NOT auto-wake either.
     * s_rtc_alarm_armed was set by arm_pcf85063_alarm_wakeup() right after
     * the /api/esp fetch, not here in the sleep path.
     * When the agent is disabled entirely, skip ALL automatic wakeup —
     * the device only wakes on button press. */
    const audio_alarm_config_t *srv_alarm = audio_get_alarm_config();
    bool user_disabled = (srv_alarm && srv_alarm->disabled);
    bool agent_off = false;
    {
        agent_config_t acfg;
        agent_off = (agent_config_load(&acfg) == ESP_OK && !acfg.enabled);
    }
    if (!s_rtc_alarm_armed && !user_disabled && !agent_off)
#endif
    {
        /* Only use the internal RTC timer when PCF85063 is absent.
         * When PCF85063 IS present but the server alarm was invalid,
         * s_rtc_alarm_armed stays false — skip ALL auto-wakeup.
         * The alarm comes exclusively from the server; no fallback
         * to CONFIG_WAKEUP_HOUR/MINUTE. */
#if CONFIG_PCF85063_ENABLE
        if (pcf85063_is_present()) {
            ESP_LOGI(TAG, "No valid server alarm, skipping timer wakeup");
        } else
#endif
        {
            time_t now = time(NULL);
            struct tm tm_now = {0}; localtime_r(&now, &tm_now);
            struct tm tm_wake = tm_now;
            tm_wake.tm_hour = CONFIG_WAKEUP_HOUR;
            tm_wake.tm_min = CONFIG_WAKEUP_MINUTE;
            tm_wake.tm_sec = 0;
            time_t wake_time = mktime(&tm_wake);
            if (wake_time <= now) wake_time += 24 * 60 * 60;
            uint64_t sleep_us = (uint64_t)(wake_time - now) * 1000000ULL;
            esp_sleep_enable_timer_wakeup(sleep_us);
            ESP_LOGI(TAG, "Timer wakeup in %llu min (%02d:%02d)",
                     (unsigned long long)(sleep_us / 60000000),
                     CONFIG_WAKEUP_HOUR, CONFIG_WAKEUP_MINUTE);
        }
    }

    // Enter deep sleep (brief delay so UART TX finishes before RTC domain powers down)
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}
