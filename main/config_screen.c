#include "config_screen.h"
#include "font_station.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "QR_PAGE";

static lv_obj_t *s_qr_root = NULL;
static lv_obj_t *s_qr      = NULL;

/* Layout (256×64) — QR left, WiFi info middle, Chinese hints right.
 *
 *   ┌────────┬──────────────────┬─────────────────────┐ y=0
 *   │  ┌──┐  │ SlumberCube-0984 │ 安睡小方 · 扫码配网  │
 *   │  │QR│  │                  │                     │
 *   │  │44│  │ setup12345678    │ 短按睡眠 · 三击重置  │
 *   │  │44│  │                  │                     │
 *   │  └──┘  │                  │                     │
 *   └────────┴──────────────────┴─────────────────────┘ y=64
 *     x=8     x=56               x=150
 *
 * font_station: line_height=18, base_line=5. ROW_H=15 is the minimum
 * that doesn't clip glyphs (10 px glyph + 2 px top + 3 px bottom).
 * CJK 10 px/char, ASCII 5 px/char. */
#define QR_SIZE     44
#define QR_X        8
#define QR_Y        10

#define MID_X       56
#define MID_W       90

#define RIGHT_X     150
#define RIGHT_W     104

#define ROW_H       15

static void build_qr_payload(const char *ssid, const char *pass,
                             char *out, size_t out_len)
{
    snprintf(out, out_len, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
}

static lv_obj_t *make_label(int32_t x, int32_t y, int32_t w,
                            const char *text, bool right_align)
{
    lv_obj_t *lbl = lv_label_create(s_qr_root);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_station, 0);
    if (right_align) {
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, text);
    lv_obj_set_size(lbl, w, ROW_H);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_scrollbar_mode(lbl, LV_SCROLLBAR_MODE_OFF);
    return lbl;
}

void config_screen_init(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGI(TAG, "Init QR: AP='%s' PASS='%s'", ap_ssid, ap_pass);

    s_qr_root = lv_obj_create(NULL);
    lv_obj_set_size(s_qr_root, 256, 64);
    lv_obj_set_style_bg_color(s_qr_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_qr_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_qr_root, 0, 0);
    lv_obj_set_style_pad_all(s_qr_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_qr_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_qr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* QR code — 44×44, left side. */
    s_qr = lv_qrcode_create(s_qr_root);
    lv_qrcode_set_dark_color(s_qr, lv_color_white());
    lv_qrcode_set_light_color(s_qr, lv_color_black());
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_quiet_zone(s_qr, false);
    lv_obj_set_pos(s_qr, QR_X, QR_Y);

    char qr_text[160];
    build_qr_payload(ap_ssid, ap_pass, qr_text, sizeof(qr_text));
    lv_qrcode_update(s_qr, qr_text, strlen(qr_text));

    /* Middle column — WiFi info next to QR. */
    make_label(MID_X, 14, MID_W, ap_ssid, false);
    make_label(MID_X, 32, MID_W, ap_pass, false);

    /* Right column — Chinese hints, right-aligned. */
    make_label(RIGHT_X, 14, RIGHT_W, "安睡小方 · 扫码配网", true);
    make_label(RIGHT_X, 32, RIGHT_W, "短按睡眠 · 三击重置", true);
}

void config_screen_show(void)
{
    if (s_qr_root == NULL) {
        ESP_LOGE(TAG, "config_screen_show before init");
        return;
    }
    lv_screen_load(s_qr_root);
}
