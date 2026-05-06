#include "weather_chart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

static const char *TAG = "WEATHER_CHART";

#define CANVAS_W 256
#define CANVAS_H 64
#define MAX_DAYS 4
#define COL_W   64

#define COL_CENTER(i) (2 + (i) * COL_W)

#define COL_DATE   0x88
#define COL_TEMP   0xBB
#define COL_LOW    0x66
#define COL_ICON   0xCC
#define COL_SEP    0x44

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static uint8_t *canvas_buf = NULL;
static lv_obj_t *date_labels[MAX_DAYS];
static lv_obj_t *high_labels[MAX_DAYS];
static lv_obj_t *low_labels[MAX_DAYS];

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

/* ── Weather icons (drawn on canvas) ── */

static void draw_weather_icon(int cx, int cy, const char *text)
{
    if (!text) return;

    if (strstr(text, "晴")) {
        /* Sun: 5px circle with 4 rays */
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
        /* Cloud: two overlapping filled circles */
        draw_filled_circle(cx - 3, cy, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 1, 4, COL_ICON);
        /* Flat bottom */
        draw_line(cx - 7, cy + 2, cx + 7, cy + 2, COL_ICON);
        draw_line(cx - 6, cy + 3, cx + 6, cy + 3, COL_ICON);

    } else if (strstr(text, "雨")) {
        /* Rain: cloud + 3 drops */
        draw_filled_circle(cx - 3, cy - 1, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 2, 4, COL_ICON);
        draw_line(cx - 7, cy + 1, cx + 7, cy + 1, COL_ICON);
        draw_line(cx - 6, cy + 2, cx + 6, cy + 2, COL_ICON);
        /* Rain drops */
        draw_line(cx - 5, cy + 4, cx - 4, cy + 8, COL_ICON);
        draw_line(cx,     cy + 5, cx + 1, cy + 9, COL_ICON);
        draw_line(cx + 5, cy + 4, cx + 6, cy + 8, COL_ICON);

    } else if (strstr(text, "雪")) {
        /* Snow: cloud + 3 asterisks */
        draw_filled_circle(cx - 3, cy - 1, 4, COL_ICON);
        draw_filled_circle(cx + 3, cy - 2, 4, COL_ICON);
        draw_line(cx - 7, cy + 1, cx + 7, cy + 1, COL_ICON);
        draw_line(cx - 6, cy + 2, cx + 6, cy + 2, COL_ICON);
        /* Snowflakes as small dots */
        draw_filled_circle(cx - 4, cy + 5, 1, COL_ICON);
        draw_filled_circle(cx,     cy + 6, 1, COL_ICON);
        draw_filled_circle(cx + 4, cy + 5, 1, COL_ICON);

    } else if (strstr(text, "雾")) {
        /* Fog: three horizontal lines */
        draw_line(cx - 6, cy - 3, cx + 6, cy - 3, COL_ICON);
        draw_line(cx - 5, cy,     cx + 5, cy,     COL_ICON);
        draw_line(cx - 6, cy + 3, cx + 6, cy + 3, COL_ICON);

    } else if (strstr(text, "风")) {
        /* Wind: angled lines */
        draw_line(cx - 5, cy - 3, cx + 5, cy - 1, COL_ICON);
        draw_line(cx - 4, cy,     cx + 4, cy + 1, COL_ICON);
        draw_line(cx - 3, cy + 2, cx + 3, cy + 4, COL_ICON);

    } else {
        /* Default: small dot */
        draw_filled_circle(cx, cy, 2, COL_ICON);
    }
}

/* ── Main draw ── */

static void draw_chart(void)
{
    if (!canvas || !canvas_buf || !weather || !weather->valid) return;

    int count = weather->count;
    if (count > MAX_DAYS) count = MAX_DAYS;
    if (count < 1) return;

    /* Clear canvas */
    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    /* Vertical separators between days */
    for (int col = 1; col < count; col++) {
        draw_line(col * COL_W, 2, col * COL_W, CANVAS_H - 1, COL_SEP);
    }

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    for (int i = 0; i < count; i++) {
        int cx = COL_CENTER(i);
        char buf[16];

        /* ── Date label ── */
        snprintf(buf, sizeof(buf), "%02d-%02d",
                 weather->daily[i].month, weather->daily[i].day);
        lv_label_set_text(date_labels[i], buf);
        lv_obj_set_pos(date_labels[i], cx, 2);
        lv_obj_set_size(date_labels[i], COL_W - 4, 10);

        /* ── Weather icon on canvas ── */
        const char *day_text = weather->daily[i].day_text;
        draw_weather_icon(cx + COL_W / 2 - 4, 20, day_text);

        /* ── High temperature ── */
        snprintf(buf, sizeof(buf), "%d°", weather->daily[i].high);
        lv_label_set_text(high_labels[i], buf);
        lv_obj_set_pos(high_labels[i], cx, 32);
        lv_obj_set_size(high_labels[i], COL_W - 4, 16);

        /* ── Low temperature ── */
        snprintf(buf, sizeof(buf), "%d°", weather->daily[i].low);
        lv_label_set_text(low_labels[i], buf);
        lv_obj_set_pos(low_labels[i], cx, 50);
        lv_obj_set_size(low_labels[i], COL_W - 4, 10);
    }

    /* Hide unused labels */
    for (int i = count; i < MAX_DAYS; i++) {
        lv_label_set_text(date_labels[i], "");
        lv_label_set_text(high_labels[i], "");
        lv_label_set_text(low_labels[i], "");
    }
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

    for (int i = 0; i < MAX_DAYS; i++) {
        date_labels[i] = lv_label_create(container);
        lv_obj_set_style_text_color(date_labels[i], lv_color_make(COL_DATE, COL_DATE, COL_DATE), 0);
        lv_obj_set_style_text_font(date_labels[i], &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_align(date_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(date_labels[i], "");

        high_labels[i] = lv_label_create(container);
        lv_obj_set_style_text_color(high_labels[i], lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
        lv_obj_set_style_text_font(high_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(high_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(high_labels[i], "");

        low_labels[i] = lv_label_create(container);
        lv_obj_set_style_text_color(low_labels[i], lv_color_make(COL_LOW, COL_LOW, COL_LOW), 0);
        lv_obj_set_style_text_font(low_labels[i], &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_align(low_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(low_labels[i], "");
    }

    return container;
}

void weather_chart_set_data(const weather_data_t *data)
{
    weather = data;
    if (!weather || !weather->valid) {
        ESP_LOGI(TAG, "set_data: invalid weather data");
        return;
    }
    ESP_LOGI(TAG, "set_data: %d days", weather->count);
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
