#include "weather_service.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "WEATHER_SVC";

/* 高德天气预报 API (daily forecast, extensions=all returns 4-day forecast) */
#define AMAP_KEY  "a2dd59be3fc29eded9ac2a4c760871e8"
#define AMAP_CITY "330100"   /* 杭州 adcode */
#define AMAP_URL  "https://restapi.amap.com/v3/weather/weatherInfo?city=" AMAP_CITY "&key=" AMAP_KEY "&extensions=all"

/* Map each daily cast to 3 hourly slots: morning, afternoon, evening */
/* 4 days × 3 = 12 slots — fits nicely into CHART_HOURS (12) */
#define SLOTS_PER_DAY 3

static const int slot_hours[SLOTS_PER_DAY] = {7, 14, 21};  /* morning, afternoon, evening */

typedef struct {
    char *buf;
    int len;
    int cap;
} resp_buf_t;

/* Derive a simple icon string from Chinese weather text */
static const char *text_to_icon(const char *text)
{
    if (!text) return "";
    if (strstr(text, "晴")) return "sun";
    if (strstr(text, "多云")) return "cloud";
    if (strstr(text, "阴")) return "overcast";
    if (strstr(text, "雾")) return "fog";
    if (strstr(text, "雪")) return "snow";
    if (strstr(text, "雨")) return "rain";
    if (strstr(text, "风")) return "wind";
    return "";
}

/* Convert day weather text to a short localized description */
static const char *text_to_short(const char *text)
{
    if (!text) return "?";
    if (strstr(text, "晴")) return "晴";
    if (strstr(text, "多云")) return "多云";
    if (strstr(text, "阴")) return "阴";
    if (strstr(text, "雾")) return "雾";
    if (strstr(text, "雪")) return "雪";
    if (strstr(text, "雨")) return "雨";
    if (strstr(text, "风")) return "风";
    return text;
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

    for (int i = 0; i < cast_count && slot_idx < WEATHER_MAX_HOURLY; i++) {
        cJSON *cast = cJSON_GetArrayItem(casts, i);
        if (!cast) continue;

        cJSON *daytemp    = cJSON_GetObjectItem(cast, "daytemp");
        cJSON *nighttemp  = cJSON_GetObjectItem(cast, "nighttemp");
        cJSON *dayweather = cJSON_GetObjectItem(cast, "dayweather");
        cJSON *nightweather = cJSON_GetObjectItem(cast, "nightweather");

        for (int s = 0; s < SLOTS_PER_DAY && slot_idx < WEATHER_MAX_HOURLY; s++) {
            bool is_night = (slot_hours[s] >= 20 || slot_hours[s] <= 6);
            const char *temp_str = is_night
                ? (nighttemp && nighttemp->valuestring ? nighttemp->valuestring : "0")
                : (daytemp && daytemp->valuestring ? daytemp->valuestring : "0");
            const char *weather_str = is_night
                ? (nightweather && nightweather->valuestring ? nightweather->valuestring : "")
                : (dayweather && dayweather->valuestring ? dayweather->valuestring : "");

            data->hourly[slot_idx].hour = slot_hours[s];

            data->hourly[slot_idx].temp     = atoi(temp_str);
            data->hourly[slot_idx].rain_prob = 0;
            data->hourly[slot_idx].rain_mm  = 0.0f;
            strncpy(data->hourly[slot_idx].icon, text_to_icon(weather_str), sizeof(data->hourly[slot_idx].icon) - 1);
            strncpy(data->hourly[slot_idx].text, text_to_short(weather_str), sizeof(data->hourly[slot_idx].text) - 1);
            slot_idx++;
        }
    }

    data->count = slot_idx;
    data->valid = true;
    data->fetch_time = time(NULL);

    if (slot_idx > 0) {
        ESP_LOGI(TAG, "Parsed %d slots from %d-day forecast, first: H%d %d°C %s",
                 slot_idx, cast_count,
                 data->hourly[0].hour, data->hourly[0].temp, data->hourly[0].text);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weather_fetch(weather_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    data->valid = false;
    data->count = 0;

    /* Allocate HTTP response buffer */
    char *resp_buf = (char *)malloc(4096);
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }
    resp_buf_t rb = { .buf = resp_buf, .len = 0, .cap = 4096 - 1 };

    esp_http_client_config_t config = {
        .url = AMAP_URL,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .skip_cert_common_name_check = true,
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
        ESP_LOGI(TAG, "Raw response: %.*s", rb.len > 200 ? 200 : rb.len, rb.buf);
        err = parse_forecast(rb.buf, rb.len, data);
    } else {
        if (err != ESP_OK) ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        if (status != 200) ESP_LOGE(TAG, "Bad HTTP status: %d", status);
        if (rb.len == 0)   ESP_LOGE(TAG, "Empty response body");
        err = ESP_FAIL;
    }

    /* Clean up */
    esp_http_client_cleanup(client);
    free(resp_buf);

    return err;
}
