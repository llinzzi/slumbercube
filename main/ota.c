/* ota.c — Over-The-Air firmware update via plain HTTP.
 *
 * The update URL is provided by the /api/esp endpoint (the "ota" JSON field).
 * This module downloads the firmware binary to the inactive OTA slot,
 * verifies it, and reboots.  Fonts are NOT included in OTA — they live in
 * a separate flash partition.
 */

#include "ota.h"
#include "clock_screen.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OTA";

/* ── Progress callback for clock_screen display ──────────────────── */

static void ota_progress(int pct) {
    char buf[32];
    snprintf(buf, sizeof(buf), "OTA: %d%%", pct);
    clock_screen_set_station_name(buf);
}

/* ── OTA implementation ──────────────────────────────────────────── */

esp_err_t ota_perform(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting OTA from: %s", url);
    ota_progress(0);

    /* ── Locate the inactive OTA partition ── */
    const esp_partition_t *update_part =
        esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA update partition found");
        clock_screen_set_station_name("OTA: no slot");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing to OTA slot: %s (offset=0x%lx size=%lu KB)",
             update_part->label,
             (unsigned long)update_part->address,
             (unsigned long)update_part->size / 1024);

    /* ── Begin OTA session ── */
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part,
                                   OTA_WITH_SEQUENTIAL_WRITES,
                                   &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        clock_screen_set_station_name("OTA: begin failed");
        return err;
    }

    /* ── HTTP download ── */
    esp_http_client_config_t http_cfg = {
        .url         = url,
        .timeout_ms  = 10000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        esp_ota_abort(ota_handle);
        clock_screen_set_station_name("OTA: http init fail");
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        clock_screen_set_station_name("OTA: connect fail");
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        clock_screen_set_station_name("OTA: HTTP err");
        return ESP_FAIL;
    }

    if (content_len <= 0 || (size_t)content_len > update_part->size) {
        ESP_LOGE(TAG, "Bad content length: %d (slot=%lu)",
                 content_len, (unsigned long)update_part->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        clock_screen_set_station_name("OTA: bad size");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloading %d bytes...", content_len);

    /* ── Streaming download + write ── */
    uint8_t *chunk = malloc(4096);
    if (!chunk) {
        ESP_LOGE(TAG, "OOM for download buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int last_pct   = -1;

    while (total_read < content_len) {
        int to_read = content_len - total_read;
        if (to_read > 4096) to_read = 4096;

        int r = esp_http_client_read(client, (char *)chunk, to_read);
        if (r <= 0) {
            ESP_LOGE(TAG, "HTTP read returned %d at %d/%d", r, total_read, content_len);
            break;
        }

        err = esp_ota_write(ota_handle, chunk, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(chunk);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            clock_screen_set_station_name("OTA: write fail");
            return err;
        }

        total_read += r;
        int pct = (total_read * 100) / content_len;
        if (pct != last_pct) {
            last_pct = pct;
            ota_progress(pct);
        }

        /* Yield to let LVGL render the progress update */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read < content_len) {
        ESP_LOGE(TAG, "Incomplete download: %d/%d", total_read, content_len);
        esp_ota_abort(ota_handle);
        clock_screen_set_station_name("OTA: incomplete");
        return ESP_FAIL;
    }

    /* ── Finalise ── */
    ota_progress(100);
    vTaskDelay(pdMS_TO_TICKS(200));

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end (verify): %s", esp_err_to_name(err));
        clock_screen_set_station_name("OTA: verify fail");
        return err;
    }

    /* ── Set boot partition & reboot ── */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        clock_screen_set_station_name("OTA: boot cfg fail");
        return err;
    }

    ESP_LOGW(TAG, "OTA complete — rebooting in 500ms");
    clock_screen_set_station_name("OTA: rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    /* Not reached */
    return ESP_OK;
}
