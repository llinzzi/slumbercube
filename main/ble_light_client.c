#include "ble_light_client.h"

#if CONFIG_BLE_LIGHT_ENABLE

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

/* These UUIDs are defined by the CH582 blinkwh firmware:
 * FFE0 = blinkwh service; FFE1 = writable LED state (0x01 turns it on). */
#define BLINKWH_SERVICE_UUID 0xFFE0
#define BLINKWH_LED_UUID     0xFFE1
#define BLINKWH_DEVICE_NAME  "blinkwh_CH"
#define BLE_LIGHT_SCAN_INTERVAL 160 /* 100 ms */
#define BLE_LIGHT_SCAN_WINDOW   160 /* continuous during the short scan */

static const char *TAG = "BLE_LIGHT";
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint16_t s_led_value_handle;
static bool s_initialized;
static bool s_synced;
static volatile bool s_requested; /* scan / connection / write in progress */
static bool s_scanning;
static bool s_connecting;
static volatile bool s_desired_on;
static bool s_command_started;
static bool s_command_on;
static uint16_t s_scan_report_count;

static int ble_light_gap_event(struct ble_gap_event *event, void *arg);
static void ble_light_start_scan(void);

static void ble_light_reset_link(void)
{
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_service_start = 0;
    s_service_end = 0;
    s_led_value_handle = 0;
    s_connecting = false;
    s_command_started = false;
}

static void ble_light_finish(uint16_t conn_handle, const char *reason)
{
    ESP_LOGI(TAG, "%s", reason);
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == BLE_HS_ENOTCONN) {
            /* No disconnect event will follow when the link is already gone. */
            s_requested = false;
            ble_light_reset_link();
        } else if (rc != 0) {
            ESP_LOGW(TAG, "disconnect failed: rc=%d", rc);
            s_requested = false;
            ble_light_reset_link();
        }
    }
}

static int ble_light_on_write(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr,
                              void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status == 0) {
        ble_light_finish(conn_handle,
                         s_command_on ? "blinkwh LED on" : "blinkwh LED off");
    } else {
        ESP_LOGW(TAG, "LED write failed: status=%d", error->status);
        ble_light_finish(conn_handle, "blinkwh LED command failed");
    }
    return 0;
}

static int ble_light_on_chr(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr,
                            void *arg)
{
    (void)arg;
    if (error->status == 0) {
        s_led_value_handle = chr->val_handle;
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "LED characteristic discovery failed: status=%d", error->status);
        ble_light_finish(conn_handle, "blinkwh LED characteristic unavailable");
        return 0;
    }

    if (s_led_value_handle == 0) {
        ble_light_finish(conn_handle, "blinkwh LED characteristic not found");
        return 0;
    }

    const uint8_t value = s_desired_on ? 0x01 : 0x00;
    s_command_on = (value != 0);
    s_command_started = true;
    int rc = ble_gattc_write_flat(conn_handle, s_led_value_handle,
                                  &value, sizeof(value), ble_light_on_write, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "LED write could not start: rc=%d", rc);
        ble_light_finish(conn_handle, "blinkwh LED write unavailable");
    }
    return 0;
}

static int ble_light_on_service(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *service,
                                void *arg)
{
    (void)arg;
    if (error->status == 0) {
        s_service_start = service->start_handle;
        s_service_end = service->end_handle;
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "service discovery failed: status=%d", error->status);
        ble_light_finish(conn_handle, "blinkwh service unavailable");
        return 0;
    }

    if (s_service_start == 0 || s_service_end == 0) {
        ble_light_finish(conn_handle, "blinkwh service not found");
        return 0;
    }

    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_service_start,
                                          s_service_end,
                                          BLE_UUID16_DECLARE(BLINKWH_LED_UUID),
                                          ble_light_on_chr, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "LED characteristic discovery could not start: rc=%d", rc);
        ble_light_finish(conn_handle, "blinkwh characteristic discovery unavailable");
    }
    return 0;
}

static bool ble_light_ad_contains_target(const uint8_t *data, uint8_t data_len)
{
    uint8_t offset = 0;
    while (offset < data_len) {
        uint8_t field_len = data[offset];
        if (field_len == 0) {
            break;
        }
        if ((uint16_t)offset + field_len + 1 > data_len) {
            return false;
        }

        uint8_t type = data[offset + 1];
        const uint8_t *value = &data[offset + 2];
        uint8_t value_len = field_len - 1;
        if (type == BLE_HS_ADV_TYPE_INCOMP_UUIDS16 ||
            type == BLE_HS_ADV_TYPE_COMP_UUIDS16) {
            for (uint8_t i = 0; i + 1 < value_len; i += 2) {
                if (value[i] == (BLINKWH_SERVICE_UUID & 0xff) &&
                    value[i + 1] == (BLINKWH_SERVICE_UUID >> 8)) {
                    return true;
                }
            }
        } else if ((type == BLE_HS_ADV_TYPE_INCOMP_NAME ||
                    type == BLE_HS_ADV_TYPE_COMP_NAME) &&
                   value_len == strlen(BLINKWH_DEVICE_NAME) &&
                   memcmp(value, BLINKWH_DEVICE_NAME, value_len) == 0) {
            return true;
        }
        offset += field_len + 1;
    }
    return false;
}

static bool ble_light_is_blinkwh(const struct ble_gap_disc_desc *disc)
{
    if (ble_light_ad_contains_target(disc->data, disc->length_data)) {
        return true;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        if (s_scan_report_count <= 5) {
            ESP_LOGW(TAG, "could not parse BLE advertisement (%u bytes)",
                     disc->length_data);
        }
        return false;
    }

    for (uint8_t i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == BLINKWH_SERVICE_UUID) {
            return true;
        }
    }

    /* The current blinkwh firmware advertises FFE0 in the primary packet.
     * Retain the complete-name match as a compatibility fallback for older
     * firmware revisions that exposed the service only after connection. */
    if (fields.name != NULL &&
        fields.name_len == strlen(BLINKWH_DEVICE_NAME) &&
        memcmp(fields.name, BLINKWH_DEVICE_NAME, fields.name_len) == 0) {
        return true;
    }
    return false;
}

static int ble_light_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        s_scan_report_count++;
        if (s_scan_report_count <= 5) {
            ESP_LOGI(TAG,
                     "scan report #%u addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d len=%u",
                     s_scan_report_count,
                     event->disc.addr.val[5], event->disc.addr.val[4],
                     event->disc.addr.val[3], event->disc.addr.val[2],
                     event->disc.addr.val[1], event->disc.addr.val[0],
                     event->disc.rssi, event->disc.length_data);
        }
        if (!s_requested || s_connecting || !ble_light_is_blinkwh(&event->disc)) {
            return 0;
        }
        s_connecting = true;
        s_scanning = false;
        (void)ble_gap_disc_cancel();
        {
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 10000,
                                     NULL, ble_light_gap_event, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "connect could not start: rc=%d", rc);
                s_connecting = false;
                s_requested = false;
            } else {
                ESP_LOGI(TAG, "connecting to blinkwh");
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (s_requested && !s_connecting) {
            ESP_LOGW(TAG, "blinkwh not found within %d ms (%u reports)",
                     CONFIG_BLE_LIGHT_SCAN_TIMEOUT_MS, s_scan_report_count);
            s_requested = false;
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            s_requested = false;
            ble_light_reset_link();
            return 0;
        }

        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected; discovering blinkwh service");
        {
            int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle,
                                                BLE_UUID16_DECLARE(BLINKWH_SERVICE_UUID),
                                                ble_light_on_service, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "service discovery could not start: rc=%d", rc);
                ble_light_finish(s_conn_handle, "blinkwh discovery unavailable");
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnected (reason=%d)", event->disconnect.reason);
        bool follow_up = s_command_started && (s_desired_on != s_command_on);
        s_requested = false;
        ble_light_reset_link();
        if (follow_up) {
            /* A newer audio state arrived while the previous GATT write was
             * in flight. Apply that latest state in a new short connection. */
            s_requested = true;
            ble_light_start_scan();
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void ble_light_start_scan(void)
{
    if (!s_synced || !s_requested || s_scanning || s_connecting) {
        return;
    }

    struct ble_gap_disc_params params = {0};
    /* Use a continuous active window for this short scan. Current blinkwh
     * firmware advertises FFE0 in the primary packet; active scanning and
     * duplicate delivery also support older revisions that expose only the
     * blinkwh_CH name in the scan response. The ESP32-C3 coexistence scheduler
     * still arbitrates Wi-Fi radio time. */
    params.itvl = BLE_LIGHT_SCAN_INTERVAL;
    params.window = BLE_LIGHT_SCAN_WINDOW;
    params.passive = 0;
    params.filter_duplicates = 1;
    s_scan_report_count = 0;
    int rc = ble_gap_disc(s_own_addr_type, CONFIG_BLE_LIGHT_SCAN_TIMEOUT_MS,
                          &params, ble_light_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan could not start: rc=%d", rc);
        s_requested = false;
        return;
    }
    s_scanning = true;
    ESP_LOGI(TAG, "scanning for blinkwh service FFE0");
}

static void ble_light_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "unable to establish BLE address: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "unable to select BLE address type: rc=%d", rc);
        return;
    }
    s_synced = true;
    ble_light_start_scan();
}

static void ble_light_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: reason=%d", reason);
    s_synced = false;
    s_scanning = false;
    ble_light_reset_link();
}

static void ble_light_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_light_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = ble_light_on_sync;
    ble_hs_cfg.reset_cb = ble_light_on_reset;
    nimble_port_freertos_init(ble_light_host_task);
    s_initialized = true;
    ESP_LOGI(TAG, "NimBLE central initialized");
    return ESP_OK;
}

static esp_err_t ble_light_set(bool on)
{
    esp_err_t err = ble_light_init();
    if (err != ESP_OK) {
        return err;
    }

    s_desired_on = on;
    if (s_requested) {
        ESP_LOGD(TAG, "blinkwh request already in progress; updated desired state");
        return ESP_OK;
    }

    s_requested = true;
    ble_light_start_scan();
    return ESP_OK;
}

esp_err_t ble_light_turn_on(void)
{
    return ble_light_set(true);
}

esp_err_t ble_light_turn_off(void)
{
    return ble_light_set(false);
}

#else

esp_err_t ble_light_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_light_turn_on(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_light_turn_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
