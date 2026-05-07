#include "weather_chart.h"
#include "weather_icons.h"
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
#define TIME_W   128
#define SEP_X    TIME_W

#define COL_DATE   0x88
#define COL_TEMP   0xCC
#define COL_LOW    0x66
#define COL_SEP    0x33
#define COL_PROGBG 0x22
#define COL_PROGFG 0xAA

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static uint8_t *canvas_buf = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *date_label = NULL;
static lv_obj_t *weather_label = NULL;
static lv_obj_t *temp_label = NULL;
static lv_obj_t *weather_date_label = NULL;
static lv_obj_t *icon_img = NULL;

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

/* ── Draw chart ── */

static void draw_chart(void)
{
    if (!canvas || !canvas_buf || !weather || !weather->valid || weather->count < 1) return;

    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    /* ── Left: Time ── */
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 (unsigned char)tm_now.tm_hour, (unsigned char)tm_now.tm_min);
        lv_label_set_text(time_label, buf);
    }
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d-%02d",
                 (unsigned char)tm_now.tm_mon + 1, (unsigned char)tm_now.tm_mday);
        lv_label_set_text(date_label, buf);
    }

    /* ── Right: Weather ── */
    lv_img_set_src(icon_img, weather_icon_match(weather->daily[0].day_text));
    lv_label_set_text(weather_label, weather->daily[0].day_text);

    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d\xc2\xb0 - %d\xc2\xb0",
                 weather->daily[0].low, weather->daily[0].high);
        lv_label_set_text(temp_label, buf);
    }
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d-%02d",
                 weather->daily[0].month, weather->daily[0].day);
        lv_label_set_text(weather_date_label, buf);
    }

    /* ── Bottom progress bar (time-of-day) ── */
    {
        int bar_y = CANVAS_H - 3;
        int bar_x0 = SEP_X + 4;
        int bar_x1 = CANVAS_W - 2;
        int bar_w = bar_x1 - bar_x0;

        draw_line(bar_x0, bar_y, bar_x1, bar_y, COL_PROGBG);

        int elapsed = tm_now.tm_hour * 60 + tm_now.tm_min;
        int total = 24 * 60;
        int fill = (bar_w * elapsed) / total;
        if (fill > 0)
            draw_line(bar_x0, bar_y, bar_x0 + fill, bar_y, COL_PROGFG);

        /* Quarter markers */
        for (int x = bar_x0 + bar_w / 4; x < bar_x1; x += bar_w / 4) {
            draw_pixel(x, bar_y - 1, COL_SEP);
            draw_pixel(x, bar_y + 1, COL_SEP);
        }
    }

    /* Bottom-left small decorative corner */
    draw_pixel(1, CANVAS_H - 2, COL_SEP);
    draw_pixel(2, CANVAS_H - 2, COL_SEP);
    draw_pixel(1, CANVAS_H - 3, COL_SEP);
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

    if (date_label) {
        char date[8];
        snprintf(date, sizeof(date), "%02d-%02d",
                 (unsigned char)tm_now.tm_mon + 1, (unsigned char)tm_now.tm_mday);
        lv_label_set_text(date_label, date);
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

    /* ── Time label (left, big digital-7) ── */
    time_label = lv_label_create(container);
    lv_obj_set_style_text_color(time_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_digital, 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(time_label, 0, 4);
    lv_obj_set_size(time_label, TIME_W, 48);
    lv_label_set_text(time_label, "");

    /* ── Date label (left, below time) ── */
    date_label = lv_label_create(container);
    lv_obj_set_style_text_color(date_label, lv_color_make(COL_DATE, COL_DATE, COL_DATE), 0);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(date_label, 0, 54);
    lv_obj_set_size(date_label, TIME_W, 10);
    lv_label_set_text(date_label, "");

    /* ── Weather icon image (right, left column) ── */
    icon_img = lv_img_create(container);
    lv_img_set_src(icon_img, weather_icon_default());
    lv_obj_set_pos(icon_img, SEP_X + 6, 4);

    /* ── Weather text (right, top) ── */
    weather_label = lv_label_create(container);
    lv_obj_set_style_text_color(weather_label, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_font(weather_label, &lv_font_weather, 0);
    lv_obj_set_pos(weather_label, SEP_X + 44, 6);
    lv_obj_set_size(weather_label, 100, 14);
    lv_label_set_text(weather_label, "");

    /* ── Temperature label (right, middle) ── */
    temp_label = lv_label_create(container);
    lv_obj_set_style_text_color(temp_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(temp_label, &lv_font_weather, 0);
    lv_obj_set_style_text_align(temp_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(temp_label, SEP_X + 44, 24);
    lv_obj_set_size(temp_label, 100, 14);
    lv_label_set_text(temp_label, "");

    /* ── Weather date label (right, bottom) ── */
    weather_date_label = lv_label_create(container);
    lv_obj_set_style_text_color(weather_date_label, lv_color_make(COL_DATE, COL_DATE, COL_DATE), 0);
    lv_obj_set_style_text_font(weather_date_label, &lv_font_weather, 0);
    lv_obj_set_style_text_align(weather_date_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(weather_date_label, SEP_X + 44, 42);
    lv_obj_set_size(weather_date_label, 100, 14);
    lv_label_set_text(weather_date_label, "");

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
