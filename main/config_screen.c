#include "config_screen.h"
#include "font_station.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "QR_PAGE";

static lv_obj_t *s_qr_root      = NULL;
static lv_obj_t *s_qr           = NULL;
static lv_obj_t *s_lbl_brand    = NULL;
static lv_obj_t *s_lbl_action   = NULL;
static lv_obj_t *s_lbl_hints    = NULL;

/* Layout (128×64) — 4 short lines so each fits in the 82-px text column:
 *
 *   ┌──────────┬──────────────────────────────────┐
 *   │          │  安睡小方                          │  y=0   (15 px)
 *   │   QR     │                                   │
 *   │  44×44   │  扫码配网                          │  y=16  (15 px)
 *   │          │                                   │
 *   │  x=0     │  短按=睡眠                         │  y=32  (15 px)
 *   │  y=10    │                                   │
 *   │          │  三击=重置                         │  y=48  (15 px)
 *   └──────────┴──────────────────────────────────┘
 *
 * font_station's monospaced CJK glyph has adv_w=80 (1/8 px) = 10 px/char.
 * "短按睡眠·三击重置" is 9 chars → 90 px, doesn't fit even in an 82-px
 * column. Splitting into "短按=睡眠" (5 chars = 50 px) and "三击=重置"
 * (5 chars = 50 px) keeps each line well under the 82-px budget.
 *
 * Label height 15 px: 9-px char + 3 px padding top + 3 px padding bottom,
 * centred vertically. 4 lines × 15 + 3 × 1 px gaps = 63 px → fits in 64. */
#define QR_SIZE  44
#define QR_X     0
#define QR_Y     10

#define TXT_X    46
#define TXT_W    82
#define LINE_H   15
#define LINE_GAP 1

/* Build the QR payload: WIFI:T:WPA;S:<ssid>;P:<pass>;;.
 * On Android & iOS, scanning this auto-joins the AP and triggers the
 * captive portal popup (no need to manually pick the network). */
static void build_qr_payload(const char *ssid, const char *pass,
                             char *out, size_t out_len)
{
    snprintf(out, out_len, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
}

/* Helper: a single-line, no-scrollbar label at (TXT_X, y) with the given
 * text. Uses the font's full line height so no glyph rows are clipped. */
static lv_obj_t *make_line(int32_t y, const char *text)
{
    lv_obj_t *lbl = lv_label_create(s_qr_root);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_station, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, text);
    lv_obj_set_size(lbl, TXT_W, LINE_H);
    lv_obj_set_pos(lbl, TXT_X, y);
    lv_obj_set_scrollbar_mode(lbl, LV_SCROLLBAR_MODE_OFF);
    return lbl;
}

void config_screen_init(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGI(TAG, "Init QR page: AP='%s'", ap_ssid);

    s_qr_root = lv_obj_create(NULL);
    lv_obj_set_size(s_qr_root, 128, 64);
    lv_obj_set_style_bg_color(s_qr_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_qr_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_qr_root, 0, 0);
    lv_obj_set_style_pad_all(s_qr_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_qr_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_qr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Left: QR. Quiet zone off so all 56 px are modules. */
    s_qr = lv_qrcode_create(s_qr_root);
    lv_qrcode_set_dark_color(s_qr, lv_color_white());
    lv_qrcode_set_light_color(s_qr, lv_color_black());
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_quiet_zone(s_qr, false);
    lv_obj_set_pos(s_qr, QR_X, QR_Y);

    char qr_text[160];
    build_qr_payload(ap_ssid, ap_pass, qr_text, sizeof(qr_text));
    lv_qrcode_update(s_qr, qr_text, strlen(qr_text));

    /* Right: brand → action → two short hint lines. 15 px tall + 1 px gap. */
    s_lbl_brand  = make_line(0,                    "安睡小方");
    s_lbl_action = make_line(0  + LINE_H + LINE_GAP, "扫码配网");
    s_lbl_hints  = make_line(0  + (LINE_H + LINE_GAP) * 2, "短按=睡眠");
    lv_obj_t *reset = make_line(0  + (LINE_H + LINE_GAP) * 3, "三击=重置");
    (void)reset;
}

void config_screen_show(void)
{
    if (s_qr_root == NULL) {
        ESP_LOGE(TAG, "config_screen_show before init");
        return;
    }
    lv_screen_load(s_qr_root);
}
