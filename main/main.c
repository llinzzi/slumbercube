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
#define EVENT_BUTTON_MASK          (EVENT_SLEEP_PENDING | EVENT_AUDIO_TOGGLE | \
                                    EVENT_PROVISIONING_REQUEST | EVENT_NEXT_TRACK)

static TaskHandle_t s_main_task = NULL;  /* set at top of app_main() */

static button_handle_t g_btn_power = NULL;
static button_handle_t g_btn_media = NULL;

static volatile bool s_audio_playing = false;   /* read by button callbacks AND main loop */
static volatile bool s_in_provisioning = false; /* read by button callbacks during captive portal */
static bool s_audio_pending           = false; /* wifi connecting, start audio when done */
static int  s_audio_pending_ticks     = 0;     /* timeout counter for pending start */
static bool s_rtc_alarm_armed          = false; /* set after arm_pcf85063_alarm_wakeup() */
static bool s_wake_from_btn           = false; /* true: IO3 wake (no network); false: RTC/sys */
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

/* ── RTC slow-memory weather cache (survives deep sleep, lost on power-off) ─── */
#define RTC_CACHE_MAGIC  0x57354348  /* "W5CH" */

RTC_DATA_ATTR static struct {
    uint32_t magic;
    int      high, low, current, humidity;
    char     day_text[16];
    char     night_text[16];
    char     current_text[16];
    int      month, day;
    time_t   fetch_time;
    /* Indoor sensor snapshot (may be NaN if no sensor) */
    float    indoor_temp;
    float    indoor_humidity;
    bool     indoor_valid;
    /* Station name for display */
    char     station_name[32];
} s_rtc_cache;

/* Reconstructed weather_data_t from RTC cache, passed to display API.
 * Allocated in DRAM (not RTC) so the pointer is valid for LVGL labels. */
static weather_data_t s_cached_weather;

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

/* ── RTC weather cache helpers ────────────────────────────────────────── */

/* Save live weather + indoor + station data to RTC slow memory.
 * Called after every successful /api/esp fetch. */
static void rtc_cache_save(const weather_data_t *w,
                           float indoor_temp, float indoor_humidity, bool indoor_valid,
                           const char *station_name)
{
    if (!w || !w->valid) return;
    s_rtc_cache.magic    = RTC_CACHE_MAGIC;
    s_rtc_cache.high     = w->daily[0].high;
    s_rtc_cache.low      = w->daily[0].low;
    s_rtc_cache.current  = w->daily[0].current;
    s_rtc_cache.humidity = w->daily[0].humidity;
    s_rtc_cache.month    = w->daily[0].month;
    s_rtc_cache.day      = w->daily[0].day;
    strncpy(s_rtc_cache.day_text,     w->daily[0].day_text,     sizeof(s_rtc_cache.day_text) - 1);
    strncpy(s_rtc_cache.night_text,   w->daily[0].night_text,   sizeof(s_rtc_cache.night_text) - 1);
    strncpy(s_rtc_cache.current_text, w->daily[0].current_text, sizeof(s_rtc_cache.current_text) - 1);
    s_rtc_cache.fetch_time    = w->fetch_time;
    s_rtc_cache.indoor_temp   = indoor_temp;
    s_rtc_cache.indoor_humidity = indoor_humidity;
    s_rtc_cache.indoor_valid  = indoor_valid;
    if (station_name && station_name[0]) {
        strncpy(s_rtc_cache.station_name, station_name, sizeof(s_rtc_cache.station_name) - 1);
    } else {
        s_rtc_cache.station_name[0] = '\0';
    }
    ESP_LOGI(TAG, "RTC cache saved: %s %d°/%d° indoor=%.1f°C",
             s_rtc_cache.day_text, s_rtc_cache.high, s_rtc_cache.low,
             indoor_valid ? (double)indoor_temp : (double)NAN);
}

/* Restore weather_data_t from RTC cache into s_cached_weather.
 * Returns true if the cache is valid (magic matches, has data). */
static bool rtc_cache_load(void)
{
    if (s_rtc_cache.magic != RTC_CACHE_MAGIC || s_rtc_cache.fetch_time == 0) {
        ESP_LOGI(TAG, "RTC cache: empty or invalid (magic=0x%08" PRIX32 ")", s_rtc_cache.magic);
        return false;
    }
    memset(&s_cached_weather, 0, sizeof(s_cached_weather));
    s_cached_weather.daily[0].high     = s_rtc_cache.high;
    s_cached_weather.daily[0].low      = s_rtc_cache.low;
    s_cached_weather.daily[0].current  = s_rtc_cache.current;
    s_cached_weather.daily[0].humidity = s_rtc_cache.humidity;
    s_cached_weather.daily[0].month    = s_rtc_cache.month;
    s_cached_weather.daily[0].day      = s_rtc_cache.day;
    strncpy(s_cached_weather.daily[0].day_text,     s_rtc_cache.day_text,     15);
    strncpy(s_cached_weather.daily[0].night_text,   s_rtc_cache.night_text,   15);
    strncpy(s_cached_weather.daily[0].current_text, s_rtc_cache.current_text, 15);
    s_cached_weather.count      = 1;
    s_cached_weather.valid      = true;
    s_cached_weather.fetch_time = s_rtc_cache.fetch_time;
    ESP_LOGI(TAG, "RTC cache loaded: %s %d°/%d° (age=%llds)",
             s_cached_weather.daily[0].day_text,
             s_cached_weather.daily[0].high, s_cached_weather.daily[0].low,
             (long long)(time(NULL) - s_rtc_cache.fetch_time));
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

    if (audio_play_url() != ESP_OK) {
        clock_screen_set_audio_indicator(false);
        clock_screen_set_station_name(audio_failure_station_name());
        s_audio_playing = false;
        return ESP_FAIL;
    }

    clock_screen_set_station_name(audio_get_station_name());
    apply_weather_and_indoor(audio_get_weather());
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

/* ── IO3 Power button callbacks ─────────────────────────────────────── */

/* IO3 short click: sleep immediately. Aborts captive portal if active. */
static void power_short_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "IO3 short click → sleep");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    if (s_main_task) xTaskNotify(s_main_task, EVENT_SLEEP_PENDING, eSetBits);
}

/* IO3 long press: cycle night mode override (auto → night → day → auto).
 * Applies immediately — no sleep required. */
static void power_long_press_cb(void *button_handle, void *usr_data)
{
    int8_t cur = clock_screen_get_night_override();
    int8_t next;
    const char *label;
    if (cur < 0)      { next = 1; label = "force NIGHT"; }
    else if (cur == 1) { next = 0; label = "force DAY"; }
    else              { next = -1; label = "AUTO (time-based)"; }

    clock_screen_set_night_override(next);
    bool is_night = clock_screen_is_night_time();
    clock_screen_set_night_mode(is_night);
    ESP_LOGI(TAG, "IO3 long press → night override: %s (night_mode=%d)", label, is_night);
}

/* IO3 triple-click: factory reset + reboot into captive portal. */
static void power_triple_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "IO3 triple click — factory reset");
    if (s_in_provisioning) {
        wifi_provisioning_abort();
    }
    if (s_main_task) xTaskNotify(s_main_task, EVENT_PROVISIONING_REQUEST, eSetBits);
}

/* ── IO1 Media button callbacks ─────────────────────────────────────── */

/* IO1 short click: toggle audio play/pause. Triggers WiFi connect + fetch
 * if not already connected. No-op in night mode or provisioning. */
static void media_short_click_cb(void *button_handle, void *usr_data)
{
#if CONFIG_AUDIO_ENABLE
    if (!s_normal_mode || clock_screen_is_night_time()) {
        ESP_LOGI(TAG, "IO1 short click — ignored (normal=%d night=%d)",
                 s_normal_mode, clock_screen_is_night_time());
        return;
    }
    ESP_LOGI(TAG, "IO1 short click → audio toggle");
    if (s_main_task) xTaskNotify(s_main_task, EVENT_AUDIO_TOGGLE, eSetBits);
    clock_screen_set_audio_indicator(!s_audio_playing);
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring IO1 short click");
#endif
}

/* IO1 long press: skip to next track. Deinit → fetch /api/esp → play. */
static void media_long_press_cb(void *button_handle, void *usr_data)
{
#if CONFIG_AUDIO_ENABLE
    if (!s_normal_mode || clock_screen_is_night_time()) {
        ESP_LOGI(TAG, "IO1 long press — ignored (normal=%d night=%d)",
                 s_normal_mode, clock_screen_is_night_time());
        return;
    }
    ESP_LOGI(TAG, "IO1 long press → next track");
    if (s_main_task) xTaskNotify(s_main_task, EVENT_NEXT_TRACK, eSetBits);
#else
    ESP_LOGI(TAG, "Audio disabled, ignoring IO1 long press");
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

    /* Detect wake source for /api/esp ?wake= query param */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_wake_from_btn = false;
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
        s_wake_from_btn = true;
        ESP_LOGI(TAG, "Woke from GPIO (IO3 button, mask=0x%llX)", (unsigned long long)wake_pins);
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

    // ── IO3 Power button (wake / sleep / night toggle / factory reset) ──
    {
        button_config_t btn_cfg = {
            .short_press_time = 300,
            .long_press_time = 1500,  /* longer to avoid accidental night toggle */
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = CONFIG_POWER_BUTTON_GPIO,
            .active_level = 0,
            .enable_power_save = false,
        };
        esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn_power);
        if (err == ESP_OK) {
            iot_button_register_cb(g_btn_power, BUTTON_SINGLE_CLICK, NULL,
                                   power_short_click_cb, NULL);
            iot_button_register_cb(g_btn_power, BUTTON_LONG_PRESS_START, NULL,
                                   power_long_press_cb, NULL);
            button_event_args_t triple_args = { .multiple_clicks.clicks = 3 };
            iot_button_register_cb(g_btn_power, BUTTON_MULTIPLE_CLICK, &triple_args,
                                   power_triple_click_cb, NULL);
            ESP_LOGI(TAG, "Power button on IO%d (short=sleep, long=night, triple=reset)",
                     CONFIG_POWER_BUTTON_GPIO);
        } else {
            ESP_LOGE(TAG, "Power button init failed on IO%d: %s",
                     CONFIG_POWER_BUTTON_GPIO, esp_err_to_name(err));
        }
    }

    // ── IO1 Media button (play/pause / next track) ──
    {
        button_config_t btn_cfg = {
            .short_press_time = 300,
            .long_press_time = 800,
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = CONFIG_MEDIA_BUTTON_GPIO,
            .active_level = 0,
            .enable_power_save = false,
        };
        esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn_media);
        if (err == ESP_OK) {
            iot_button_register_cb(g_btn_media, BUTTON_SINGLE_CLICK, NULL,
                                   media_short_click_cb, NULL);
            iot_button_register_cb(g_btn_media, BUTTON_LONG_PRESS_START, NULL,
                                   media_long_press_cb, NULL);
            ESP_LOGI(TAG, "Media button on IO%d (short=play/pause, long=next)",
                     CONFIG_MEDIA_BUTTON_GPIO);
        } else {
            ESP_LOGE(TAG, "Media button init failed on IO%d: %s",
                     CONFIG_MEDIA_BUTTON_GPIO, esp_err_to_name(err));
        }
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

#if CONFIG_PCF85063_ENABLE
    pcf85063_init();
    apply_pcf85063_time();
#endif

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());
    log_heap("lvgl_init");

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

        /* ── Networking strategy ──────────────────────────────────────────
         * IO3 wake  → no WiFi, no fetch, display cached weather from RTC
         * RTC wake  → WiFi + /api/esp fetch + save cache + auto-play audio
         * IO1 press → on-demand WiFi + fetch + play (handled in main loop) */

        if (s_wake_from_btn) {
            /* ── IO3 button wake: zero-network path ── */
            ESP_LOGI(TAG, "IO3 wake — cached weather, no network");
            if (rtc_cache_load()) {
                screens_set_weather_data_ptr(&s_cached_weather);
                if (s_rtc_cache.station_name[0]) {
                    clock_screen_set_station_name(s_rtc_cache.station_name);
                }
                if (s_rtc_cache.indoor_valid) {
                    clock_screen_set_indoor_env(s_rtc_cache.indoor_temp,
                                                s_rtc_cache.indoor_humidity);
                }
            } else {
                clock_screen_set_station_name("Press IO1 for radio");
            }
            /* Read live indoor sensor for this session */
            {
                float t = 0, h = 0;
                if (read_indoor_env(&t, &h)) {
                    clock_screen_set_indoor_env(t, h);
                }
            }
        } else {
            /* ── RTC / cold-boot wake: full network + weather + audio ── */
            clock_screen_set_station_name("Connecting WiFi...");
            wifi_ensure_netif();
            if (wifi_init_sta() == ESP_OK) {
                if (!wifi_is_time_set()) {
                    wifi_mark_time_set();
                }
#if CONFIG_PCF85063_ENABLE
                if (pcf85063_is_present()) pcf85063_sync_from_system();
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
            ESP_LOGI(TAG, "Night mode, skipping network and weather");
        }
#else
        ESP_LOGI(TAG, "Night mode, skipping network and weather");
#endif
    }

#if CONFIG_AUDIO_ENABLE
    /* ── Auto-audio only on RTC/sys wake (not IO3 button wake) ── */
    if (!s_wake_from_btn && s_normal_mode && !clock_screen_is_night_time()) {
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

            /* Save fresh weather to RTC cache immediately */
            if (got_weather) {
                const weather_data_t *w = audio_get_weather();
                rtc_cache_save(w, t, h, got_indoor,
                               audio_get_station_name());
            }

#if CONFIG_PCF85063_ENABLE
            s_rtc_alarm_armed = arm_pcf85063_alarm_wakeup();
#endif
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
                clock_screen_set_station_name("Starting audio...");
            }
            vTaskDelay(pdMS_TO_TICKS(50));

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

    // ── Main loop: event-driven with 1s tick timeout ──
    for (uint32_t tick = 0; tick < (uint32_t)CONFIG_ACTIVE_DURATION_SECS; tick++) {
        uint32_t notified = 0;
        xTaskNotifyWait(0, EVENT_BUTTON_MASK, &notified, pdMS_TO_TICKS(1000));

        /* IO3 short click → sleep */
        if (notified & EVENT_SLEEP_PENDING) {
            break;
        }

        /* IO3 triple-click → factory reset */
        if (notified & EVENT_PROVISIONING_REQUEST) {
            do_factory_reset();
        }

#if CONFIG_AUDIO_ENABLE
        /* IO1 short click → toggle audio (on-demand WiFi if needed) */
        if (notified & EVENT_AUDIO_TOGGLE) {
            if (s_audio_playing) {
                audio_stop();
                clock_screen_set_audio_indicator(false);
                clock_screen_set_station_name("Paused");
                s_audio_playing = false;
                /* Clear cached URL so next play does a fresh /api/esp fetch */
            } else if (wifi_is_connected()) {
                audio_start_playback(false);
            } else {
                /* WiFi down → connect + fetch + play */
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

        /* IO1 long press → next track (skip current, fetch new from /api/esp) */
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
                    rtc_cache_save(w, t, h, got_indoor,
                                   audio_get_station_name());
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
                stall_ticks = 0;
                audio_deinit();
                ESP_LOGI(TAG, "Audio deinit, fetching next song...");
                vTaskDelay(pdMS_TO_TICKS(1000));

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
                        rtc_cache_save(w, t2, h2, gi,
                                       audio_get_station_name());
                        if (w && w->valid) {
                            screens_set_weather_data_ptr(w);
                        }
                        if (gi) {
                            audio_set_indoor_env(t2, h2);
                            clock_screen_set_indoor_env(t2, h2);
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

        /* Refresh indoor temp every 10s */
        if (tick % 10 == 0) {
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
