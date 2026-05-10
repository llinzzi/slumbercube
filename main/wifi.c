#include "wifi.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/apps/sntp.h"

static const char *TAG = "WIFI";

#define WIFI_SSID     "Happy"
#define WIFI_PASS     "ping8275"
#define MAX_RETRY     10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_wifi_connected = false;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START received");
        esp_err_t err = esp_wifi_connect();
        ESP_LOGI(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(err));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num == 0 || s_retry_num >= MAX_RETRY || s_retry_num % 5 == 0) {
            ESP_LOGI(TAG, "WIFI_DISCONNECTED, retry=%d", s_retry_num);
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

static void sntp_init_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.nist.gov");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_init();

    // Wait for time to be set, polling up to 10 seconds
    int retry = 0;
    const int max_retry = 20;
    time_t now = 0;
    struct tm timeinfo = {0};
    for (retry = 0; retry < max_retry; retry++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2020 - 1900)) {
            ESP_LOGI(TAG, "SNTP sync success (%dms): %04d-%02d-%02d %02d:%02d:%02d",
                     (retry + 1) * 500,
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return;
        }
    }
    ESP_LOGW(TAG, "SNTP sync failed after %dms, time: %ld", max_retry * 500, (long)now);
}

static bool s_netif_inited = false;

esp_err_t wifi_ensure_netif(void)
{
    if (s_netif_inited) return ESP_OK;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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

esp_err_t wifi_init_sta(void)
{
    if (s_wifi_inited) {
        /* Already initialized — just trigger reconnect if disconnected */
        if (!s_wifi_connected) {
            ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
            if (wifi_wait_connected(30000) == ESP_OK) return ESP_OK;
            ESP_LOGW(TAG, "WiFi reconnect timeout (30s)");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    wifi_ensure_netif();

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, "oled_clock"));

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

    wifi_config_t wifi_config = {
        .sta = {
            .threshold = {
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
            .channel = 1,  /* Skip full channel scan, connect faster */
        },
    };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_max_tx_power(50);

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    s_wifi_inited = true;

    if (wifi_wait_connected(30000) == ESP_OK) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
        sntp_init_time();
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi connection timeout (30s)");
    return ESP_FAIL;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

static const char *NVS_NAMESPACE = "clock";
static const char *NVS_KEY_TIME_SET = "time_set";

bool wifi_is_time_set(void)
{
    // Initialize NVS first (required after deep sleep reboot)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
    }

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
