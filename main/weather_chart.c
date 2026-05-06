#include "weather_chart.h"
#include "font_weather.h"
#include "font_digital.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

static const char *TAG = "WEATHER_CHART";

#define CANVAS_W 256
#define CANVAS_H 64
#define TIME_W   88
#define WEATHER_X 90

#define COL_DATE   0x88
#define COL_TEMP   0xBB
#define COL_LOW    0x66
#define COL_ICON   0xCC
#define COL_SEP    0x33

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static uint8_t *canvas_buf = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *weather_label = NULL;
static lv_obj_t *temp_label = NULL;

static const weather_data_t *weather = NULL;
static bool visible = false;

/* ── Canvas drawing helpers ── */

static void draw_pixel(int x, int y, uint8_t gray)
{
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
    canvas_buf[y * CANVAS_W + x] = gray;
}

static void draw_line(int x0, int y0, int x1, int y1, uint8_t gray)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        draw_pixel(x0, y0, gray);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void draw_filled_circle(int cx, int cy, int r, uint8_t gray)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r)
                draw_pixel(x, y, gray);
        }
    }
}

/* ── Weather icons ── */

static void draw_weather_icon(int cx, int cy, const char *text)
{
    if (!text) return;

    if (strstr(text, "晴")) {
        draw_filled_circle(cx, cy, 2, COL_ICON);
        draw_pixel(cx, cy - 5, COL_ICON);
        draw_pixel(cx, cy + 5, COL_ICON);
        draw_pixel(cx - 5, cy, COL_ICON);
        draw_pixel(cx + 5, cy, COL_ICON);
        draw_pixel(cx - 3, cy - 3, COL_ICON);
        draw_pixel(cx + 3, cy - 3, COL_ICON);
        draw_pixel(cx - 3, cy + 3, COL_ICON);
        draw_pixel(cx + 3, cy + 3, COL_ICON);

    } else if (strstr(text, "云") || strstr(text, "阴")) {
        draw_filled_circle(cx - 3, cy, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 1, 4, COL_ICON);
        draw_line(cx - 7, cy + 2, cx + 7, cy + 2, COL_ICON);
        draw_line(cx - 6, cy + 3, cx + 6, cy + 3, COL_ICON);

    } else if (strstr(text, "雨")) {
        draw_filled_circle(cx - 3, cy - 1, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 2, 4, COL_ICON);
        draw_line(cx - 7, cy + 1, cx + 7, cy + 1, COL_ICON);
        draw_line(cx - 6, cy + 2, cx + 6, cy + 2, COL_ICON);
        draw_line(cx - 5, cy + 4, cx - 4, cy + 8, COL_ICON);
        draw_line(cx,     cy + 5, cx + 1, cy + 9, COL_ICON);
        draw_line(cx + 5, cy + 4, cx + 6, cy + 8, COL_ICON);

    } else if (strstr(text, "雪")) {
        draw_filled_circle(cx - 3, cy - 1, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 2, 4, COL_ICON);
        draw_line(cx - 7, cy + 1, cx + 7, cy + 1, COL_ICON);
        draw_line(cx - 6, cy + 2, cx + 6, cy + 2, COL_ICON);
        draw_filled_circle(cx - 4, cy + 5, 1, COL_ICON);
        draw_filled_circle(cx,     cy + 6, 1, COL_ICON);
        draw_filled_circle(cx + 4, cy + 5, 1, COL_ICON);

    } else if (strstr(text, "雾")) {
        draw_line(cx - 6, cy - 3, cx + 6, cy - 3, COL_ICON);
        draw_line(cx - 5, cy,     cx + 5, cy,     COL_ICON);
        draw_line(cx - 6, cy + 3, cx + 6, cy + 3, COL_ICON);

    } else if (strstr(text, "风")) {
        draw_line(cx - 5, cy - 3, cx + 5, cy - 1, COL_ICON);
        draw_line(cx - 4, cy,     cx + 4, cy + 1, COL_ICON);
        draw_line(cx - 3, cy + 2, cx + 3, cy + 4, COL_ICON);

    } else {
        draw_filled_circle(cx, cy, 2, COL_ICON);
    }
}

/* ── Draw chart ── */

static void draw_chart(void)
{
    if (!canvas || !canvas_buf || !weather || !weather->valid || weather->count < 1) return;

    /* Clear canvas */
    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    /* Separator between time and weather */
    draw_line(TIME_W, 4, TIME_W, CANVAS_H - 4, COL_SEP);

    /* Current time on the left */
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 (unsigned char)tm_now.tm_hour, (unsigned char)tm_now.tm_min);
        lv_label_set_text(time_label, buf);
    }

    /* Today's weather on the right */
    draw_weather_icon(116, 10, weather->daily[0].day_text);
    lv_label_set_text(weather_label, weather->daily[0].day_text);

    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d° / %d°",
                 weather->daily[0].high, weather->daily[0].low);
        lv_label_set_text(temp_label, buf);
    }
}

void weather_chart_update_time(void)
{
    if (!visible || !time_label) return;

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d",
             (unsigned char)tm_now.tm_hour, (unsigned char)tm_now.tm_min);
    lv_label_set_text(time_label, buf);
}

/* ── Public API ── */

lv_obj_t *weather_chart_create(lv_obj_t *parent)
{
    container = lv_obj_create(parent);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, CANVAS_W, CANVAS_H);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);

    canvas_buf = heap_caps_malloc(CANVAS_W * CANVAS_H, MALLOC_CAP_8BIT);
    if (!canvas_buf) {
        ESP_LOGE(TAG, "OOM for canvas buffer (%d)", CANVAS_W * CANVAS_H);
        return container;
    }
    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    canvas = lv_canvas_create(container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_L8);
    lv_obj_set_pos(canvas, 0, 0);

    /* Time label (left side) */
    time_label = lv_label_create(container);
    lv_obj_set_style_text_color(time_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_digital, 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(time_label, 0, 14);
    lv_obj_set_size(time_label, TIME_W, 36);
    lv_label_set_text(time_label, "");

    /* Weather text label (right side, next to icon) */
    weather_label = lv_label_create(container);
    lv_obj_set_style_text_color(weather_label, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_font(weather_label, &lv_font_weather, 0);
    lv_obj_set_pos(weather_label, 140, 6);
    lv_obj_set_size(weather_label, 100, 20);
    lv_label_set_text(weather_label, "");

    /* Temp label (right side) */
    temp_label = lv_label_create(container);
    lv_obj_set_style_text_color(temp_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(temp_label, WEATHER_X, 22);
    lv_obj_set_size(temp_label, CANVAS_W - WEATHER_X - 4, 24);
    lv_label_set_text(temp_label, "");

    return container;
}

void weather_chart_set_data(const weather_data_t *data)
{
    weather = data;
    if (!weather || !weather->valid) {
        ESP_LOGI(TAG, "set_data: invalid weather data");
        return;
    }
    ESP_LOGI(TAG, "set_data: %s %d°/%d°",
             weather->daily[0].day_text,
             weather->daily[0].high, weather->daily[0].low);
    draw_chart();
    lv_obj_invalidate(container);
    lv_refr_now(lv_disp_get_default());
}

void weather_chart_show(void)
{
    if (!container) return;
    visible = true;
    if (weather && weather->valid) {
        draw_chart();
    }
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(container);
}

void weather_chart_hide(void)
{
    if (!container) return;
    visible = false;
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

bool weather_chart_is_visible(void)
{
    return visible;
}
