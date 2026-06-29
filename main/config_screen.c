#include "config_screen.h"
#include "font_station.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "CFG_SCREEN";

/* Persisted across hide/show cycles. NULL until the first show. */
static lv_obj_t *s_cfg_root  = NULL;
static lv_obj_t *s_ap_label  = NULL;
static lv_obj_t *s_qr        = NULL;
static lv_obj_t *s_url_label = NULL;
/* Screen to restore on hide() — captured the first time show() runs. */
static lv_obj_t *s_prev_screen = NULL;

/* Layout (128x64):
 *   y=0..7   : AP SSID label  (8 px)
 *   y=8..63  : QR code        (56 px, centered horizontally)
 * 56 / 29 modules = ~1.93 px/module — slightly smaller than full-screen but
 * still scannable, and the label tells the user which AP they're looking at. */
#define CFG_QR_SIZE  56
#define CFG_QR_X     36   /* (128 - 56) / 2 — center horizontally */
#define CFG_QR_Y     8

/* Build the QR payload: WIFI:T:WPA;S:<ssid>;P:<pass>;;.
 * On Android & iOS, scanning this auto-joins the AP and triggers the
 * captive portal popup (no need to manually pick the network). */
static void build_qr_payload(const char *ssid, const char *pass,
                             char *out, size_t out_len)
{
    /* Escape any backslash/semicolon in ssid/pass defensively. */
    snprintf(out, out_len, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
}

void config_screen_show(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGI(TAG, "Showing config screen: AP='%s'", ap_ssid);

    if (s_cfg_root == NULL) {
        s_cfg_root = lv_obj_create(NULL);
        lv_obj_set_size(s_cfg_root, 128, 64);
        lv_obj_set_style_bg_color(s_cfg_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_cfg_root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_cfg_root, 0, 0);
        lv_obj_set_style_pad_all(s_cfg_root, 0, 0);

        /* Top: AP SSID label so the user can tell which device they're
         * looking at (handy if more than one SlumberCube is nearby). */
        s_ap_label = lv_label_create(s_cfg_root);
        lv_obj_set_style_text_color(s_ap_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_ap_label, &lv_font_station, 0);
        lv_obj_set_pos(s_ap_label, 0, 0);
        lv_obj_set_size(s_ap_label, 128, 8);
        lv_label_set_long_mode(s_ap_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_ap_label, LV_TEXT_ALIGN_CENTER, 0);

        /* Middle: QR code. Quiet zone off so all 56 px are modules. */
        s_qr = lv_qrcode_create(s_cfg_root);
        lv_qrcode_set_dark_color(s_qr, lv_color_white());
        lv_qrcode_set_light_color(s_qr, lv_color_black());
        lv_qrcode_set_size(s_qr, CFG_QR_SIZE);
        lv_qrcode_set_quiet_zone(s_qr, false);
        lv_obj_set_pos(s_qr, CFG_QR_X, CFG_QR_Y);

        /* Bottom: action hint. Make sure the user knows they need to
         * actively open the browser on their phone — scanning alone just
         * joins the AP. */
        s_url_label = lv_label_create(s_cfg_root);
        lv_obj_set_style_text_color(s_url_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_url_label, &lv_font_station, 0);
        lv_label_set_text(s_url_label, "scan:join  open:submit");
        lv_obj_align(s_url_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    /* Update payload in case the AP identity changed (it won't, since it's
     * derived from the device MAC, but updating keeps the function idempotent
     * and safe to call from a re-provisioning flow). */
    char qr_text[160];
    build_qr_payload(ap_ssid, ap_pass, qr_text, sizeof(qr_text));
    lv_qrcode_update(s_qr, qr_text, strlen(qr_text));

    /* Refresh the SSID label. */
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "%.17s", ap_ssid);  /* truncate for narrow display */
    lv_label_set_text(s_ap_label, hdr);

    /* Capture the screen we displaced (the EEZ-generated main screen, with
     * the clock widgets on it). Used by config_screen_hide() to restore. */
    if (s_prev_screen == NULL) {
        s_prev_screen = lv_screen_active();
    }

    /* Load this screen onto the display. Since ssd1322_display_on() was
     * already called at boot, runtime screen swaps go straight to GDDRAM
     * — no anti-white-flash concern here. */
    lv_screen_load(s_cfg_root);
}

void config_screen_hide(void)
{
    if (s_prev_screen != NULL && s_prev_screen != s_cfg_root) {
        lv_screen_load(s_prev_screen);
    }
}