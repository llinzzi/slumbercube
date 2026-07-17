#include "wifi.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

static const char *TAG = "WIFI";

/* WiFi 配置通过 menuconfig 设置 (参见 Kconfig.projbuild) */
#define MAX_RETRY     10

static char s_device_id[13] = {0};  /* hex MAC, e.g. "543204470984" */

/* Set to true during provisioning to keep the wifi event handler from
 * auto-connecting to a stale STA while the AP is up. */
static volatile bool s_suppress_auto_connect = false;

const char *wifi_get_device_id(void)
{
    if (s_device_id[0] == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Device ID: %s", s_device_id);
    }
    return s_device_id;
}

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_wifi_connected = false;
/* True when wifi_sta_connect() is using NVS-saved creds (vs. the menuconfig
 * fallback). Used to distinguish "wrong password" (NVS creds, recoverable
 * via factory reset → re-provisioning) from "first-boot probe of menuconfig
 * fallback" (no creds yet, provisioning already handles it). */
static bool s_using_nvs_creds = false;
static bool s_wifi_started = false;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_suppress_auto_connect) {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START suppressed (provisioning)");
            return;
        }
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START received");
        esp_err_t err = esp_wifi_connect();
        ESP_LOGI(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(err));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_suppress_auto_connect) {
            ESP_LOGI(TAG, "WIFI_DISCONNECTED suppressed (provisioning)");
            return;
        }
        /* Always log the disconnect reason — without it, instability (e.g.
         * AP-side 3-second kick) is a guessing game. */
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WIFI_DISCONNECTED reason=%d (rssi=%d) retry=%d",
                 (int)disc->reason, disc->rssi, s_retry_num);

        /* Self-heal: if NVS-saved creds repeatedly fail 4-way handshake, the
         * password is wrong (or the AP changed). Bouncing the radio for the
         * full 30s wait buys nothing — nuke NVS and reboot into the captive
         * portal so the user can re-enter creds. Skip when s_using_nvs_creds
         * is false (NVS is empty, no creds to try, no menuconfig fallback
         * anymore) — that case is
         * already inside the provisioning flow. */
        if (s_using_nvs_creds && s_retry_num >= 2 &&
            (disc->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
             disc->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
             disc->reason == WIFI_REASON_802_1X_AUTH_FAILED ||
             disc->reason == WIFI_REASON_AUTH_FAIL)) {
            ESP_LOGE(TAG, "NVS creds rejected by AP after %d retries (reason=%d) — "
                          "erasing NVS and rebooting into provisioning",
                     s_retry_num, (int)disc->reason);
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
        } else {
            ESP_LOGW(TAG, "WiFi max retry reached, giving up");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_set_timezone(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (UTC+8)");
}

void wifi_suppress_auto_connect(bool suppress)
{
    s_suppress_auto_connect = suppress;
    ESP_LOGI(TAG, "auto-connect %s", suppress ? "suppressed" : "enabled");
}

void wifi_mark_radio_started(void)
{
    s_wifi_started = true;
}

static bool s_sntp_started = false;
/* Set true the first time SNTP's underlying poll successfully applies a
 * server response to the system clock. Flipped back to false on every
 * (re-)initialisation. Used by callers that want to wait until a real
 * NTP round-trip has happened before trusting time() to be authoritative
 * — important when persisting the system clock to PCF85063, otherwise we
 * may overwrite a freshly-fixed RTC with the pre-sync (boot-time) value. */
static volatile bool s_sntp_synced_since_init = false;

static void sntp_sync_cb(struct timeval *tv)
{
    s_sntp_synced_since_init = true;
    ESP_LOGI(TAG, "SNTP sync notification received (offset %lld.%06lds from epoch)",
             (long long)tv->tv_sec, (long)tv->tv_usec);
}

bool wifi_is_time_synced(void)
{
    return s_sntp_synced_since_init;
}

static void sntp_init_time(void)
{
    if (s_sntp_started) {
        ESP_LOGI(TAG, "SNTP already running, skipping init");
        return;
    }
    /* PCF85063 already set the system clock at boot, so we don't need to
     * block on SNTP. Fire-and-forget: start the NTP client in the background;
     * lwIP runs SNTP polls on its own timer and corrects the clock gradually.
     * The next wake cycle will sync PCF85063 from the corrected time. */
    s_sntp_synced_since_init = false;
    ESP_LOGI(TAG, "Starting SNTP (background), poll interval 15 s");

    wifi_set_timezone();
    /* NTP server choice. We've tried pool.ntp.org / time.nist.gov (often
     * DNS-hijacked on CN networks) and the aliyun IPs 120.25.108.11 /
     * 203.107.6.88 (UDP/123 outbound to those IPs is blocked on at least
     * one user network). pool.ntp.org is the most universally reachable
     * for ESP32 projects — verified working from both a Mac workstation
     * and the ESP32 itself on the same network. The second entry
     * (time.windows.com) is a public fallback Microsoft runs. */
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp1.aliyun.com");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    /* Default SNTP poll interval is 1 hour — useless for a wake-cycle
     * worker that gives up after 30 s. Reset to 15 s so a single wake
     * can capture a reply even if the first poll's DNS resolution or
     * UDP attempt is delayed. SNTPv4 RFC 4330 enforces a 15 s minimum. */
    esp_sntp_set_sync_interval(15000);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    s_sntp_started = true;
}

static bool s_netif_inited = false;

esp_err_t wifi_ensure_netif(void)
{
    if (s_netif_inited) return ESP_OK;

    /* NVS is initialised centrally in app_main() — no need to re-init here. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_inited = true;
    ESP_LOGI(TAG, "Netif initialized");
    return ESP_OK;
}

static bool s_wifi_inited = false;

static esp_err_t wifi_wait_connected(int timeout_ms)
{
    int elapsed = 0;
    const int tick = 100;
    while (elapsed < timeout_ms) {
        if (s_wifi_connected) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(tick));
        elapsed += tick;
    }
    return ESP_FAIL;
}

/* Non-blocking: init + configure + start the STA radio.
 * Returns ESP_OK when the radio is started and the driver will attempt
 * to connect automatically (WIFI_EVENT_STA_START → esp_wifi_connect()).
 * The caller polls wifi_is_connected() to detect when the link comes up.
 *
 * Safe to call when already connected (idempotent fast path). */
esp_err_t wifi_sta_ensure(void)
{
    /* First-time path: init the driver + STA netif + event handlers. */
    if (!s_wifi_inited) {
        wifi_ensure_netif();

        s_wifi_event_group = xEventGroupCreate();

        esp_netif_t *netif = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_netif_set_hostname(netif, "ssd1322"));

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_got_ip));

        s_wifi_inited = true;
        ESP_LOGI(TAG, "wifi driver initialized");
    }

    /* Idempotent fast path: radio already up and connected. */
    if (s_wifi_started && s_wifi_connected) {
        return ESP_OK;
    }

    s_retry_num     = 0;
    s_wifi_connected = false;

    wifi_config_t wifi_config = {
        .sta = {
            .threshold = { .authmode = WIFI_AUTH_WPA2_PSK },
            .channel    = 1,  /* Skip full channel scan, connect faster */
        },
    };

    wifi_creds_t creds;
    if (wifi_creds_load(&creds) == ESP_OK) {
        s_using_nvs_creds = true;
        strncpy((char *)wifi_config.sta.ssid,     creds.ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, creds.pass, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Using NVS creds for SSID:'%s'", creds.ssid);
    } else {
        s_using_nvs_creds = false;
        ESP_LOGW(TAG, "NVS creds missing, refusing to connect without provisioning");
        return ESP_FAIL;
    }

    if (s_wifi_started) {
        /* Re-init: radio is running with stale config. Stop + restart. */
        ESP_LOGI(TAG, "Restarting WiFi to apply new config");
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Use near-max TX power (80 = 20 dBm). */
        esp_wifi_set_max_tx_power(80);

        s_wifi_started = true;
    }

    return ESP_OK;
}

/* Blocking wrapper: ensure + wait for connection. Pure wifi — does NOT
 * touch the SNTP client. The audio path and the NTP worker each
 * decide independently whether to start SNTP via wifi_start_sntp(). */
esp_err_t wifi_sta_connect(uint32_t timeout_ms)
{
    esp_err_t err = wifi_sta_ensure();
    if (err != ESP_OK) return err;

    if (wifi_wait_connected(timeout_ms) == ESP_OK) {
        ESP_LOGI(TAG, "connected to ap");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi connection timeout (%u ms)", (unsigned)timeout_ms);
    return ESP_FAIL;
}

/* App-layer entry point for kicking off SNTP after wifi is up. Returns
 * ESP_ERR_INVALID_STATE if wifi hasn't connected yet — caller is expected
 * to sequence the wifi-init step (alarm-wake path / button-press NTP
 * worker) before calling this. Idempotent: returns ESP_OK without
 * re-init if SNTP is already running. */
esp_err_t wifi_start_sntp(void)
{
    if (s_sntp_started) {
        ESP_LOGI(TAG, "SNTP already running");
        return ESP_OK;
    }
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "wifi_start_sntp called before wifi connected");
        return ESP_ERR_INVALID_STATE;
    }
    sntp_init_time();
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

static const char *NVS_NAMESPACE = "clock";
static const char *NVS_KEY_TIME_SET = "time_set";

bool wifi_is_time_set(void)
{
    /* NVS is initialised centrally in app_main(). */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS '%s' namespace not found (%s), time not set", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, NVS_KEY_TIME_SET, &value);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS time_set=%u -> time IS set", value);
    } else {
        ESP_LOGI(TAG, "NVS time_set not found (%s) -> time NOT set", esp_err_to_name(err));
    }

    return (err == ESP_OK && value == 1);
}

void wifi_mark_time_set(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(handle, NVS_KEY_TIME_SET, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set NVS value: %s", esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "Time set flag saved to NVS");
    }
    nvs_close(handle);
}

/* ---- WiFi credential storage (NVS namespace "wifi_cfg") ------------------
 * Kept separate from the "clock" namespace used by wifi_is_time_set so a
 * factory reset of creds doesn't disturb other settings. */
static const char *WIFI_CREDS_NAMESPACE = "wifi_cfg";
static const char *WIFI_CREDS_KEY_SSID  = "ssid";
static const char *WIFI_CREDS_KEY_PASS  = "pass";
static const char *WIFI_CREDS_KEY_FLAG  = "configured";

esp_err_t wifi_creds_load(wifi_creds_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* NVS is initialised centrally in app_main(). */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('%s') failed: %s", WIFI_CREDS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    /* configured flag is the sentinel — if missing, treat as unprovisioned. */
    uint8_t configured = 0;
    err = nvs_get_u8(handle, WIFI_CREDS_KEY_FLAG, &configured);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "nvs_get_u8(configured) failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t ssid_len = sizeof(out->ssid);
    size_t pass_len = sizeof(out->pass);
    err = nvs_get_str(handle, WIFI_CREDS_KEY_SSID, out->ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "nvs_get_str(ssid) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_get_str(handle, WIFI_CREDS_KEY_PASS, out->pass, &pass_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "nvs_get_str(pass) failed: %s", esp_err_to_name(err));
        return err;
    }
    out->configured = (configured == 1);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t wifi_creds_save(const wifi_creds_t *c)
{
    if (c == NULL || c->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('%s') failed: %s", WIFI_CREDS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_CREDS_KEY_SSID, c->ssid);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, WIFI_CREDS_KEY_PASS, c->pass);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u8(handle, WIFI_CREDS_KEY_FLAG, 1);
    if (err != ESP_OK) goto fail;
    err = nvs_commit(handle);
    if (err != ESP_OK) goto fail;

    nvs_close(handle);
    ESP_LOGI(TAG, "Credentials saved to NVS (SSID:'%s')", c->ssid);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "wifi_creds_save failed at step: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}
