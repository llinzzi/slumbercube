#include "wifi.h"
#include <string.h>
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
            s_retry_num = 0;
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void sntp_init_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    // Set timezone to CST (China Standard Time, UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();

    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.nist.gov");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_init();

    // Wait for time to be set (simple delay approach)
    vTaskDelay(pdMS_TO_TICKS(3000));

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2020 - 1900)) {
        ESP_LOGI(TAG, "SNTP sync success: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync may have failed, time: %ld", (long)now);
    }
}

esp_err_t wifi_init_sta(void)
{
    // Initialize NVS for WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
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
            .channel = 0,
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

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
        sntp_init_time();
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}
