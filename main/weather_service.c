#include "weather_service.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "miniz_tinfl.h"
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "WEATHER_SVC";

#define QWEATHER_URL "https://nn3aaqw4wr.re.qweatherapi.com/v7/weather/24h?location=101210101&key=e8e879aca230481f9201f67de0583184"

/* Static decompressor in BSS — avoids heap metadata corruption from tinfl */
static tinfl_decompressor s_decomp;

typedef struct {
    char *buf;
    int len;
    int cap;
} resp_buf_t;

static char *decompress_gzip(const char *src, int src_len, int *out_len)
{
    const unsigned char *uc = (const unsigned char *)src;
    if (src_len < 18 || uc[0] != 0x1f || uc[1] != 0x8b) {
        ESP_LOGE(TAG, "Invalid gzip data (magic=0x%02x%02x)", src[0], src[1]);
        return NULL;
    }

    /* Parse gzip header: skip optional extra fields (FLG byte 3) */
    int hdr_size = 10;
    uint8_t flg = uc[3];
    if (flg & 0x04) { int xlen = uc[hdr_size] | (uc[hdr_size + 1] << 8); hdr_size += 2 + xlen; }
    if (flg & 0x08) { while (hdr_size < src_len && uc[hdr_size] != 0) hdr_size++; hdr_size++; }
    if (flg & 0x10) { while (hdr_size < src_len && uc[hdr_size] != 0) hdr_size++; hdr_size++; }
    if (flg & 0x02) { hdr_size += 2; }

    int deflate_len = src_len - hdr_size - 8;
    if (deflate_len <= 0) {
        ESP_LOGE(TAG, "No deflate data (hdr=%d deflate=%d)", hdr_size, deflate_len);
        return NULL;
    }

    /* Use static decompressor (BSS, not heap) to avoid heap metadata corruption */
    tinfl_decompressor *decomp = &s_decomp;
    tinfl_init(decomp);

    size_t out_cap = 8192;
    size_t guard = 1024;
    char *out = (char *)malloc(out_cap + guard);
    if (!out) { ESP_LOGE(TAG, "No mem for output"); return NULL; }

    size_t in_ofs = 0, out_ofs = 0;
    tinfl_status status;
    do {
        size_t in_avail = deflate_len - in_ofs;
        size_t out_avail = out_cap - out_ofs;
        status = tinfl_decompress(decomp,
            (const mz_uint8 *)src + hdr_size + in_ofs, &in_avail,
            (mz_uint8 *)out, (mz_uint8 *)out + out_ofs,
            &out_avail, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        in_ofs += in_avail;
        out_ofs += out_avail;
        if (status == TINFL_STATUS_DONE) break;
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT) {
            ESP_LOGE(TAG, "tinfl status=%d in=%d out=%d", status, (int)in_ofs, (int)out_ofs);
            free(out); return NULL;
        }
        if (out_ofs >= out_cap) {
            ESP_LOGE(TAG, "Output buffer full at %d", (int)out_ofs);
            free(out); return NULL;
        }
    } while (1);

    ESP_LOGI(TAG, "Decompressed %d -> %zu bytes", deflate_len, out_ofs);
    out[out_ofs] = '\0';
    *out_len = (int)out_ofs;
    return out;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb && evt->data) {
                int needed = rb->len + evt->data_len;
                if (needed + 1 > rb->cap) {
                    ESP_LOGW(TAG, "Response buffer full (%d/%d)", rb->len, rb->cap);
                    break;
                }
                memcpy(rb->buf + rb->len, evt->data, evt->data_len);
                rb->len = needed;
                rb->buf[rb->len] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP fetch complete, %d bytes", rb ? rb->len : 0);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t parse_hourly(const char *json, int json_len, weather_data_t *data)
{
    const char *err_ptr = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &err_ptr, false);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed at offset %d",
                 err_ptr ? (int)(err_ptr - json) : -1);
        return ESP_FAIL;
    }

    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!cJSON_IsArray(hourly)) {
        ESP_LOGE(TAG, "No hourly array in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(hourly);
    if (count > WEATHER_MAX_HOURLY) count = WEATHER_MAX_HOURLY;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(hourly, i);
        if (!item) continue;

        cJSON *fxTime  = cJSON_GetObjectItem(item, "fxTime");
        cJSON *temp    = cJSON_GetObjectItem(item, "temp");
        cJSON *pop     = cJSON_GetObjectItem(item, "pop");
        cJSON *precip  = cJSON_GetObjectItem(item, "precip");
        cJSON *text    = cJSON_GetObjectItem(item, "text");
        cJSON *icon    = cJSON_GetObjectItem(item, "icon");

        if (fxTime && cJSON_IsString(fxTime) && fxTime->valuestring) {
            int h, m, s;
            if (sscanf(fxTime->valuestring, "%*d-%*d-%*dT%d:%d:%d", &h, &m, &s) >= 1) {
                data->hourly[i].hour = h;
            }
        }
        if (temp && cJSON_IsString(temp) && temp->valuestring) {
            data->hourly[i].temp = atoi(temp->valuestring);
        }
        if (pop && cJSON_IsString(pop) && pop->valuestring) {
            data->hourly[i].rain_prob = atoi(pop->valuestring);
        }
        if (precip && cJSON_IsString(precip) && precip->valuestring) {
            data->hourly[i].rain_mm = (float)atof(precip->valuestring);
        }
        if (text && cJSON_IsString(text) && text->valuestring) {
            strncpy(data->hourly[i].text, text->valuestring, sizeof(data->hourly[i].text) - 1);
        }
        if (icon && cJSON_IsString(icon) && icon->valuestring) {
            strncpy(data->hourly[i].icon, icon->valuestring, sizeof(data->hourly[i].icon) - 1);
        }
    }

    data->count = count;
    data->valid = true;
    data->fetch_time = time(NULL);

    if (count > 0) {
        ESP_LOGI(TAG, "Parsed %d hours, first: H%d %d°C pop=%d%%",
                 count, data->hourly[0].hour, data->hourly[0].temp, data->hourly[0].rain_prob);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weather_fetch(weather_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    data->valid = false;
    data->count = 0;

    char *resp_buf = (char *)malloc(4096);
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }
    resp_buf_t rb = { .buf = resp_buf, .len = 0, .cap = 4096 - 1 };

    esp_http_client_config_t config = {
        .url = QWEATHER_URL,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .skip_cert_common_name_check = true,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(resp_buf);
        return ESP_FAIL;
    }

    char *uncomp = NULL;
    int uncomp_len = 0;
    bool need_free_uncomp = false;

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status=%d, body=%d bytes", status, rb.len);
        if (status == 200 && rb.len > 0) {
            const unsigned char *d = (const unsigned char *)rb.buf;
            bool is_gzip = (rb.len >= 2 && d[0] == 0x1f && d[1] == 0x8b);
            if (is_gzip) {
                uncomp = decompress_gzip(rb.buf, rb.len, &uncomp_len);
                need_free_uncomp = true;
                if (!uncomp) {
                    ESP_LOGE(TAG, "Decompression failed");
                    err = ESP_FAIL;
                }
            } else {
                /* Not gzipped — use response buffer directly (cannot free before parse) */
                uncomp = rb.buf;
                uncomp_len = rb.len;
            }
        } else {
            ESP_LOGE(TAG, "Bad HTTP response: %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    /* Free HTTP resources before cJSON parse to maximize available heap */
    esp_http_client_cleanup(client);
    client = NULL;
    free(resp_buf);
    resp_buf = NULL;

    /* Now parse with freed-up heap */
    if (uncomp) {
        err = parse_hourly(uncomp, uncomp_len, data);
        if (need_free_uncomp) free(uncomp);
    }

    return err;
}
