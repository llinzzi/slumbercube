#include "main_screen.h"
#include "weather_icons.h"
#include "loading_img.h"
#include "u8g2_adapter.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

static const char *TAG = "MAIN_SCREEN";

extern const uint8_t u8g2_font_digital7_mono_42_tn[];

#define CANVAS_W 256
#define CANVAS_H 64
#define TIME_W   128
#define SEP_X    128

#define COL_DATE   0x88
#define COL_TEMP   0xCC
#define COL_SEP    0x33
#define COL_PROGBG 0x22
#define COL_PROGFG 0xAA

static const weather_data_t *s_weather = NULL;

/* ── L8 icon blit helper ── */

static void draw_l8_image(u8g2_t *u8g2, const uint8_t *data,
                           int x, int y, int w, int h)
{
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            uint8_t pixel = data[py * w + px];
            if (pixel == 0) continue;
            u8g2_SetDrawColor(u8g2, pixel >> 4);
            u8g2_DrawPixel(u8g2, x + px, y + py);
        }
    }
}

/* ── Screen elements ── */

static void draw_separator(u8g2_t *u8g2)
{
    u8g2_SetDrawColor(u8g2, COL_SEP);
    u8g2_DrawLine(u8g2, SEP_X, 0, SEP_X, CANVAS_H - 1);
}

static void draw_time(u8g2_t *u8g2)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    char hour_buf[8], min_buf[8], full_buf[16];
    snprintf(hour_buf, sizeof(hour_buf), "%02d:", (unsigned char)tm_now.tm_hour);
    snprintf(min_buf, sizeof(min_buf), "%02d", (unsigned char)tm_now.tm_min);
    snprintf(full_buf, sizeof(full_buf), "%02d:%02d",
             (unsigned char)tm_now.tm_hour, (unsigned char)tm_now.tm_min);

    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_digital7_mono_42_tn);

    int full_w = u8g2_GetStrWidth(u8g2, full_buf);
    int hour_w = u8g2_GetStrWidth(u8g2, hour_buf);
    int x0 = (TIME_W - full_w) / 2;

    /* Hours in lighter gray, minutes in full brightness */
    u8g2_SetDrawColor(u8g2, 0x66);
    u8g2_DrawStr(u8g2, x0, 46, hour_buf);
    u8g2_SetDrawColor(u8g2, COL_TEMP);
    u8g2_DrawStr(u8g2, x0 + hour_w, 46, min_buf);
}

static void draw_date(u8g2_t *u8g2)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d-%02d",
             (unsigned char)tm_now.tm_mon + 1, (unsigned char)tm_now.tm_mday);

    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetDrawColor(u8g2, COL_DATE);
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(u8g2, (TIME_W - u8g2_GetStrWidth(u8g2, buf)) / 2, 60, buf);
}

static void draw_loading(u8g2_t *u8g2)
{
    const uint8_t *data = loading_img_get();
    int x = SEP_X + (128 - LOADING_IMG_W) / 2;
    int y = (CANVAS_H - LOADING_IMG_H) / 2;
    draw_l8_image(u8g2, data, x, y, LOADING_IMG_W, LOADING_IMG_H);
}

static void draw_weather(u8g2_t *u8g2)
{
    if (!s_weather || !s_weather->valid || s_weather->count < 1) return;

    /* Weather icon */
    const uint8_t *icon = weather_icon_match(s_weather->daily[0].day_text);
    draw_l8_image(u8g2, icon, SEP_X + 6, 4, WEATHER_ICON_W, WEATHER_ICON_H);

    /* Temperature */
    char buf[24];
    snprintf(buf, sizeof(buf), "%d-%d",
             s_weather->daily[0].low, s_weather->daily[0].high);

    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetDrawColor(u8g2, COL_TEMP);
    u8g2_SetFont(u8g2, u8g2_font_7x14_tf);
    u8g2_DrawStr(u8g2, SEP_X + 44, 24 + 14, buf);

    /* Weather date */
    snprintf(buf, sizeof(buf), "%02d-%02d",
             s_weather->daily[0].month, s_weather->daily[0].day);

    u8g2_SetDrawColor(u8g2, COL_DATE);
    u8g2_SetFont(u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(u8g2, SEP_X + 44, 42 + 8, buf);
}

static void draw_progress_bar(u8g2_t *u8g2)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    int bar_y = CANVAS_H - 3;
    int bar_x0 = SEP_X + 4;
    int bar_x1 = CANVAS_W - 2;
    int bar_w = bar_x1 - bar_x0;

    /* Background */
    u8g2_SetDrawColor(u8g2, COL_PROGBG);
    u8g2_DrawLine(u8g2, bar_x0, bar_y, bar_x1, bar_y);

    /* Fill */
    int elapsed = tm_now.tm_hour * 60 + tm_now.tm_min;
    int total = 24 * 60;
    int fill = (bar_w * elapsed) / total;
    if (fill > 0) {
        u8g2_SetDrawColor(u8g2, COL_PROGFG);
        u8g2_DrawLine(u8g2, bar_x0, bar_y, bar_x0 + fill, bar_y);
    }

    /* Quarter markers */
    for (int x = bar_x0 + bar_w / 4; x < bar_x1; x += bar_w / 4) {
        u8g2_SetDrawColor(u8g2, COL_SEP);
        u8g2_DrawPixel(u8g2, x, bar_y - 1);
        u8g2_DrawPixel(u8g2, x, bar_y + 1);
    }
}

static void draw_bottom_left_corner(u8g2_t *u8g2)
{
    u8g2_SetDrawColor(u8g2, COL_SEP);
    u8g2_DrawPixel(u8g2, 1, CANVAS_H - 2);
    u8g2_DrawPixel(u8g2, 2, CANVAS_H - 2);
    u8g2_DrawPixel(u8g2, 1, CANVAS_H - 3);
}

static void main_screen_redraw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);

    draw_separator(u8g2);
    draw_time(u8g2);
    draw_date(u8g2);
    draw_bottom_left_corner(u8g2);

    if (s_weather && s_weather->valid) {
        draw_weather(u8g2);
    } else {
        draw_loading(u8g2);
    }

    draw_progress_bar(u8g2);

    u8g2_SendBuffer(u8g2);
}

/* ── Public API ── */

void main_screen_init(void)
{
    u8g2_t *u8g2 = u8g2_adapter_get_handle();
    main_screen_redraw(u8g2);
    ESP_LOGI(TAG, "Main screen initialized");
}

void main_screen_set_weather_data(const weather_data_t *data)
{
    s_weather = data;
    if (!s_weather || !s_weather->valid) {
        ESP_LOGI(TAG, "set_weather_data: invalid data");
        return;
    }
    ESP_LOGI(TAG, "set_weather_data: %s %d/%d",
             s_weather->daily[0].day_text,
             s_weather->daily[0].high, s_weather->daily[0].low);

    u8g2_t *u8g2 = u8g2_adapter_get_handle();
    main_screen_redraw(u8g2);
}

void main_screen_tick(void)
{
    u8g2_t *u8g2 = u8g2_adapter_get_handle();
    main_screen_redraw(u8g2);
}
