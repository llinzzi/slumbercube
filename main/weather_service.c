#include "weather_service.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "WEATHER_SVC";

/* 高德天气预报 API — 配置通过 menuconfig 设置 */
#define AMAP_URL_FMT "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=all"

typedef struct {
    char *buf;
    int len;
    int cap;
} resp_buf_t;

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

/* Parse "2026-05-07" into month=5, day=7 */
static void parse_date(const char *date_str, int *month, int *day)
{
    *month = 1; *day = 1;
    if (!date_str) return;
    int y, m, d;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) == 3) {
        *month = m;
        *day = d;
    }
}

static esp_err_t parse_forecast(const char *json, int json_len, weather_data_t *data)
{
    const char *err_ptr = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &err_ptr, false);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed at offset %d",
                 err_ptr ? (int)(err_ptr - json) : -1);
        return ESP_FAIL;
    }

    /* Check API status */
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status || !cJSON_IsString(status) || strcmp(status->valuestring, "1") != 0) {
        cJSON *info = cJSON_GetObjectItem(root, "info");
        ESP_LOGE(TAG, "高德 API error: status=%s info=%s",
                 status ? status->valuestring : "null",
                 info && info->valuestring ? info->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *forecasts = cJSON_GetObjectItem(root, "forecasts");
    if (!cJSON_IsArray(forecasts) || cJSON_GetArraySize(forecasts) < 1) {
        ESP_LOGE(TAG, "No forecasts array in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *forecast = cJSON_GetArrayItem(forecasts, 0);
    cJSON *casts = cJSON_GetObjectItem(forecast, "casts");
    if (!cJSON_IsArray(casts)) {
        ESP_LOGE(TAG, "No casts array in forecast");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int cast_count = cJSON_GetArraySize(casts);
    int slot_idx = 0;

    for (int i = 0; i < cast_count && slot_idx < WEATHER_MAX_DAYS; i++) {
        cJSON *cast = cJSON_GetArrayItem(casts, i);
        if (!cast) continue;

        cJSON *date_json      = cJSON_GetObjectItem(cast, "date");
        cJSON *daytemp        = cJSON_GetObjectItem(cast, "daytemp");
        cJSON *nighttemp      = cJSON_GetObjectItem(cast, "nighttemp");
        cJSON *dayweather     = cJSON_GetObjectItem(cast, "dayweather");
        cJSON *nightweather   = cJSON_GetObjectItem(cast, "nightweather");

        parse_date(date_json && date_json->valuestring ? date_json->valuestring : NULL,
                   &data->daily[slot_idx].month,
                   &data->daily[slot_idx].day);

        data->daily[slot_idx].high = daytemp && daytemp->valuestring
            ? atoi(daytemp->valuestring) : 0;
        data->daily[slot_idx].low  = nighttemp && nighttemp->valuestring
            ? atoi(nighttemp->valuestring) : 0;

        strncpy(data->daily[slot_idx].day_text,
                dayweather && dayweather->valuestring ? dayweather->valuestring : "",
                sizeof(data->daily[slot_idx].day_text) - 1);
        strncpy(data->daily[slot_idx].night_text,
                nightweather && nightweather->valuestring ? nightweather->valuestring : "",
                sizeof(data->daily[slot_idx].night_text) - 1);

        slot_idx++;
    }

    data->count = slot_idx;
    data->valid = (slot_idx > 0);
    data->fetch_time = time(NULL);

    if (data->valid) {
        for (int i = 0; i < slot_idx; i++) {
            ESP_LOGI(TAG, "  Day%d: %02d-%02d  %d/%d°C  %s",
                     i,
                     data->daily[i].month, data->daily[i].day,
                     data->daily[i].high, data->daily[i].low,
                     data->daily[i].day_text);
        }
    }

    cJSON_Delete(root);
    return data->valid ? ESP_OK : ESP_FAIL;
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

    /* Build API URL from Kconfig values */
    char api_url[256];
    snprintf(api_url, sizeof(api_url), AMAP_URL_FMT, CONFIG_AMAP_CITY, CONFIG_AMAP_KEY);

    esp_http_client_config_t config = {
        .url = api_url,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(resp_buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    }
    ESP_LOGI(TAG, "HTTP result=%s status=%d body=%d bytes",
             esp_err_to_name(err), status, rb.len);

    if (err == ESP_OK && status == 200 && rb.len > 0) {
        err = parse_forecast(rb.buf, rb.len, data);
    } else {
        if (err != ESP_OK) ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        if (status != 200) ESP_LOGE(TAG, "Bad HTTP status: %d", status);
        if (rb.len == 0)   ESP_LOGE(TAG, "Empty response body");
        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(resp_buf);
    return err;
}
