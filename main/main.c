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
#include "esp_mac.h"
#include "nvs_flash.h"
#include <time.h>
#include "iot_button.h"
#include "button_gpio.h"
#include "shtc3.h"

#if CONFIG_PCF85063_ENABLE
#include "pcf85063.h"
#include <sys/time.h>
#endif

#if CONFIG_AUDIO_ENABLE
#include "audio_player_wrapper.h"
#endif

static const char *TAG = "MAIN";

/* 配置项通过 menuconfig 设置 (参见 Kconfig.projbuild) */

static button_handle_t g_btn = NULL;
static volatile bool s_sleep_pending = false;

static volatile bool s_audio_playing = false;
static volatile bool s_audio_toggle_request = false;
static volatile bool s_provisioning_request = false;   /* triple-click → reconfig */
static volatile bool s_in_provisioning    = false;     /* true while captive portal is up */
static bool s_rtc_alarm_armed          = false;     /* set after arm_pcf85063_alarm_wakeup() */
static bool s_normal_mode              = false;     /* true only when we reached the
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

    /* Server explicitly disabled the alarm — honour it. Leave PCF85063
     * registers untouched (whatever they were set to last cycle) and do not
     * raise the IO0 wake pin. The caller will then also skip the internal
     * timer fallback, leaving only the button to wake the device. */
    const audio_alarm_config_t *srv = audio_get_alarm_config();
    if (srv && srv->disabled) {
        ESP_LOGW(TAG, "PCF85063: server disabled alarm, skipping arm");
        return false;
    }

    /* Values from Kconfig (CONFIG_WAKEUP_HOUR/MINUTE) and from the server
     * are both local time (CST, UTC+8). PCF85063 stores UTC internally and
     * compares alarm registers against its UTC clock — convert to UTC. */
    uint8_t wake_h = (uint8_t)(((int)CONFIG_WAKEUP_HOUR + 24 - 8) % 24);
    uint8_t wake_m = CONFIG_WAKEUP_MINUTE;
    if (srv && srv->valid) {
        wake_h = (uint8_t)(((int)srv->hour + 24 - 8) % 24);
        wake_m = srv->minute;
        ESP_LOGI(TAG, "PCF85063: server alarm %02d:%02d CST -> %02d:%02d UTC",
                 srv->hour, srv->minute, wake_h, wake_m);
    }
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

/* Short click: sleep. If we're currently in the captive portal (which is
 * blocking the main loop in wifi_provisioning_run()), unblock it first so
 * the sleep path can run; otherwise the press would be ignored for up to
 * CONFIG_WIFI_PROV_TIMEOUT_SECS. */
static void button_short_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Short click, going to sleep");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    s_sleep_pending = true;
}

/* Long press: request audio toggle. Main loop handles WiFi + playback. */
static void button_long_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Long press");
#if CONFIG_AUDIO_ENABLE
    s_audio_toggle_request = true;
    /* Show indicator immediately when starting, hide when stopping */
    clock_screen_set_audio_indicator(!s_audio_playing);
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring long press");
#endif
}

/* Triple-click: factory reset. Erase all NVS and reboot. The next boot
 * sees no NVS creds and falls into the captive portal automatically —
 * a single state machine (factory-reset → portal → reboot → normal).
 *
 * If the captive portal is currently up (which is blocking the main loop
 * in wifi_provisioning_run()), abort it first so the reset path can run
 * within a second or two instead of waiting for the 5-minute timeout. */
static void button_triple_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Triple click — factory reset");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    s_provisioning_request = true;
}

/* Wipe NVS and reboot. Shared by the main-loop triple-click handler and the
 * boot-time provisioning path (so triple-click during the first-boot
 * captive portal takes effect right after the function returns, not after
 * a 5-min timeout). */
static void do_factory_reset(void)
{
    s_provisioning_request = false;
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
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", CONFIG_ACTIVE_DURATION_SECS);

    /* Detect wake source for /api/esp ?wake= query param */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        audio_set_wake_source("rtc");
        ESP_LOGI(TAG, "Woke from RTC timer");
        break;
    case ESP_SLEEP_WAKEUP_GPIO: {
        uint64_t wake_pins = esp_sleep_get_gpio_wakeup_status();
#if CONFIG_PCF85063_ENABLE
        if (wake_pins & (1ULL << CONFIG_PCF85063_INT_GPIO)) {
            audio_set_wake_source("rtc");
            ESP_LOGI(TAG, "Woke from PCF85063 alarm (IO%d)", CONFIG_PCF85063_INT_GPIO);
            pcf85063_clear_alarm_flag();
            break;
        }
#endif
        audio_set_wake_source("btn");
        ESP_LOGI(TAG, "Woke from GPIO (button, mask=0x%llX)", (unsigned long long)wake_pins);
        break;
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        audio_set_wake_source("sys");
        ESP_LOGI(TAG, "Cold boot");
        break;
    default:
        audio_set_wake_source("sys");
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

    // Initialize SSD1322 driver first (display stays OFF until first frame rendered)
    ESP_ERROR_CHECK(ssd1322_init());

    // Initialize button with short/long press distinction
    // - short_press_time 300ms: 容忍机械抖动，普通点击就能识别
    // - long_press_time  800ms: 更短的长按识别，用户体验更灵敏
    button_config_t btn_cfg = {
        .short_press_time = 300,
        .long_press_time = 800,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_BUTTON_GPIO,
        .active_level = 0,
        .enable_power_save = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);
    if (err == ESP_OK) {
        iot_button_register_cb(g_btn, BUTTON_SINGLE_CLICK, NULL, button_short_click_cb, NULL);
        iot_button_register_cb(g_btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, NULL);
        /* Triple-click opens WiFi provisioning UI (user reconfigurable). */
        button_event_args_t triple_args = { .multiple_clicks.clicks = 3 };
        iot_button_register_cb(g_btn, BUTTON_MULTIPLE_CLICK, &triple_args,
                               button_triple_click_cb, NULL);
        ESP_LOGI(TAG, "Button initialized on GPIO%d (short=toggle, long=sleep, triple=WiFi setup)",
                 CONFIG_BUTTON_GPIO);
    } else {
        ESP_LOGE(TAG, "Failed to init button on GPIO%d: %s", CONFIG_BUTTON_GPIO, esp_err_to_name(err));
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

#if CONFIG_PCF85063_ENABLE
    pcf85063_init();
    apply_pcf85063_time();
#endif

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

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

    /* Force a synchronous flush so the active screen is in GDDRAM *before*
     * we turn the panel on. The lvgl_task is running at 10 ms intervals and
     * may have queued a flush of the default (now black) screen; the sync
     * flush here runs immediately and overwrites GDDRAM with the real screen.
     * This is the second half of the anti-white-flash fix (the first half
     * is painting the default screen black in lvgl_adapter_init). */
    lv_refr_now(lv_disp_get_default());

    // Turn on display AFTER first frame is in GDDRAM — eliminates white flash on wake
    ssd1322_display_on();

    // Wait for UI to load
    vTaskDelay(pdMS_TO_TICKS(100));

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
             * and sets s_provisioning_request. Honour it now — before any
             * wifi/audio init — so the user doesn't sit through 30s of
             * menuconfig-fallback retries. */
            if (s_provisioning_request) {
                do_factory_reset();
            }
            ESP_LOGW(TAG, "Provisioning did not save creds, clock-only mode (no WiFi)");
            s_normal_mode = false;  /* no creds → no audio, no SHTC3 */
        }
#endif

        // Always init TCP/IP stack + start WiFi (needed for /api/esp)
        clock_screen_set_station_name("Connecting WiFi...");
        wifi_ensure_netif();
        if (wifi_init_sta() == ESP_OK) {
            if (!wifi_is_time_set()) {
                wifi_mark_time_set();
            }
#if CONFIG_PCF85063_ENABLE
            if (pcf85063_is_present()) pcf85063_sync_from_system();
#endif
        }
        /* Weather is fetched as part of audio_play_url() via /api/esp;
         * the display shows the default icon until that completes. */
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
            if (s_provisioning_request) {
                do_factory_reset();
            }
        } else {
            ESP_LOGI(TAG, "Night mode, skipping network and weather fetch");
        }
#else
        ESP_LOGI(TAG, "Night mode, skipping network and weather fetch");
#endif
    }

#if CONFIG_AUDIO_ENABLE
    /* Start audio playback (non-blocking: mixer + decoder + HTTP tasks run in background).
     * Only run after we have a real WiFi connection — the QR provisioning page
     * and clock-only mode should not touch I2S (audio amp) or the SHTC3 sensor.
     * Skipping these on the provisioning page also makes the device silent
     * during the captive-portal "scan to connect" flow. */
    if (s_normal_mode && !clock_screen_is_night_time()) {
#if CONFIG_PCF85063_ENABLE
        if (should_skip_alarm_today()) {
            ESP_LOGI(TAG, "Weekend — alarm suppressed, clock-only wake");
            clock_screen_set_station_name("Weekend");
        } else
#endif
        {
        /* Read indoor sensor before any network call */
        float t = 0, h = 0;
        if (shtc3_read(&t, &h)) {
            audio_set_indoor_env(t, h);
            ESP_LOGI(TAG, "SHTC3: indoor %.1f°C, %.0f%%RH", t, h);
        }

        if (audio_init() == ESP_OK) {
            /* 1. Single HTTP call — parses both weather and radio URL from /api/esp.
             * Weather and URL are independent; one can succeed without the other.
             * audio_fetch_api() returns ESP_ERR_NOT_SUPPORTED when the agent is
             * disabled by config — clock-only mode, skip weather + radio. */
            clock_screen_set_station_name("Fetching weather...");
            esp_err_t fetch_rc = audio_fetch_api();
            bool got_weather = (fetch_rc == ESP_OK);

            /* Arm PCF85063 alarm immediately after parsing the server
             * response — don't wait until deep sleep. The alarm registers
             * are written now; only the GPIO wake-mask + hold are applied
             * later in the sleep path. */
#if CONFIG_PCF85063_ENABLE
            s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
            if (fetch_rc == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGI(TAG, "Agent disabled by config, clock-only mode");
                /* Override the "Fetching weather..." placeholder — without
                 * this, the label stays stuck because the success path
                 * below is skipped. */
                clock_screen_set_station_name(audio_failure_station_name());
            }

            /* Display whatever we got (best-effort) */
            const weather_data_t *w = audio_get_weather();
            if (got_weather && w && w->valid) {
                screens_set_weather_data_ptr(w);
            }
            clock_screen_set_indoor_env(t, h);

            if (got_weather) {
                clock_screen_set_station_name("Starting audio...");
            }
            /* Give LVGL one tick to render before audio starts buffering */
            vTaskDelay(pdMS_TO_TICKS(50));

            /* 2. Start audio — only if the same API response included a URL.
             * audio_fetch_api() already cached s_radio_url above. */
            if (audio_radio_url_is_set()) {
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

    // Main loop: button press or timeout → sleep
    for (int i = 0; i < CONFIG_ACTIVE_DURATION_SECS; i++) {
        if (s_sleep_pending) {
            break;
        }

        /* Handle triple-click factory reset. Wipe NVS and reboot — the next
         * boot sees no saved creds and falls into the captive portal. */
        if (s_provisioning_request) {
            do_factory_reset();
        }

#if CONFIG_AUDIO_ENABLE
        /* Handle long-press audio toggle request */
        if (s_audio_toggle_request) {
            s_audio_toggle_request = false;
            if (s_audio_playing) {
                audio_stop();
                clock_screen_set_audio_indicator(false);
                clock_screen_set_station_name("Paused");
                s_audio_playing = false;
            } else {
                clock_screen_set_station_name("Connecting WiFi...");
                wifi_ensure_netif();
                if (wifi_init_sta() == ESP_OK) {
                    clock_screen_set_station_name("Starting audio...");
                    if (audio_init() == ESP_OK) {
                        if (audio_play_url() == ESP_OK) {
                            clock_screen_set_station_name(audio_get_station_name());
                            apply_weather_and_indoor(audio_get_weather());
                            clock_screen_set_audio_indicator(true);
                            s_audio_playing = true;
                        } else {
                            clock_screen_set_station_name(audio_failure_station_name());
                        }
                    }
                } else {
                    /* wifi_init_sta timed out — check if it connected just after timeout */
                    if (wifi_is_connected()) {
                        clock_screen_set_station_name("Starting audio...");
                        if (audio_init() == ESP_OK) {
                            if (audio_play_url() == ESP_OK) {
                                clock_screen_set_station_name(audio_get_station_name());
                                apply_weather_and_indoor(audio_get_weather());
                                clock_screen_set_audio_indicator(true);
                                s_audio_playing = true;
                            } else {
                                clock_screen_set_station_name(audio_failure_station_name());
                            }
                        }
                    } else {
                        clock_screen_set_audio_indicator(false);
                        clock_screen_set_station_name("WiFi failed");
                    }
                }
            }
        }

        /* Auto-advance: when a track finishes, fetch the next song from /api/esp
         * and continue playing in a loop.
         *
         * Two conditions trigger advance:
         * 1. Stream state goes IDLE (clean finish)
         * 2. HTTP download at 100% for 3+ seconds (decoder stuck on trailing garbage) */
        if (s_audio_playing && !s_audio_toggle_request) {
            int progress = audio_get_progress();
            static int stall_ticks = 0;
            bool track_done = false;

            if (audio_is_finished()) {
                const char *name = audio_get_station_name();
                ESP_LOGI(TAG, "Track finished cleanly: '%s', advancing to next", name ? name : "unknown");
                track_done = true;
            } else if (progress >= 100) {
                stall_ticks++;
                ESP_LOGW(TAG, "Download complete but decoder stalled for %d s", (int)stall_ticks);
                if (stall_ticks >= 3) {
                    ESP_LOGW(TAG, "Decoder stalled, force-advancing to next track");
                    track_done = true;
                }
            } else {
                stall_ticks = 0;
            }

            if (track_done) {
                stall_ticks = 0;
                audio_deinit();
                ESP_LOGI(TAG, "Audio deinitialized, fetching next song...");
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* Re-init I2S (deinit tore it down), then fetch next song and play */
                clock_screen_set_station_name("Next song...");
                if (!wifi_is_connected()) {
                    wifi_init_sta();
                }
                if (audio_init() == ESP_OK) {
                    if (audio_play_url() == ESP_OK) {
                        clock_screen_set_station_name(audio_get_station_name());
                        apply_weather_and_indoor(audio_get_weather());
                        clock_screen_set_audio_indicator(true);
                    } else {
                        ESP_LOGW(TAG, "Failed to fetch next track, stopping");
                        clock_screen_set_audio_indicator(false);
                        clock_screen_set_station_name("Paused");
                        s_audio_playing = false;
                    }
                } else {
                    ESP_LOGW(TAG, "Audio re-init failed, stopping");
                    clock_screen_set_audio_indicator(false);
                    clock_screen_set_station_name("Paused");
                    s_audio_playing = false;
                }
            }
        }

#endif

        /* Refresh indoor temp every 10s — updates display + cache for next /api/esp */
        if (i % 10 == 0) {
            float t = 0, h = 0;
            if (shtc3_read(&t, &h)) {
                audio_set_indoor_env(t, h);
                clock_screen_set_indoor_env(t, h);
            }
        }

        /* Full-screen refresh every second. Serialised against
         * lvgl_task via g_lvgl_mutex inside lvgl_adapter_refr_now(). */
        lvgl_adapter_refr_now();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Time to sleep, turning off display");

    /* Kill display and amp instantly so user sees/hears immediate shutdown.
     * Audio cleanup (stopping the HTTP download task) takes a few seconds
     * and must happen after these to avoid visible delay. */
    ssd1322_display_off();
    gpio_set_level(CONFIG_PIN_NS4168_CTRL, 0);

#if CONFIG_AUDIO_ENABLE
    audio_stop();
    audio_deinit();
#endif

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
     * the /api/esp fetch, not here in the sleep path. */
    const audio_alarm_config_t *srv_alarm = audio_get_alarm_config();
    bool user_disabled = (srv_alarm && srv_alarm->disabled);
    if (!s_rtc_alarm_armed && !user_disabled)
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

    // Enter deep sleep (brief delay so UART TX finishes before RTC domain powers down)
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}
