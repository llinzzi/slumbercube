#include "clock_screen.h"
#include "font_digital.h"
#include "font_station.h"
#include "audio_player_wrapper.h"
#include "ssd1322_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "WEATHER_CHART";

#define CANVAS_W 256
#define CANVAS_H 64
#define TIME_W   128
#define SEP_X    TIME_W

#define COL_DATE   0xFF
#define COL_TEMP   0xFF
#define COL_LOW    0xFF
#define COL_SEP    0xFF

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static uint8_t *canvas_buf = NULL;
/* Time area: five labels. Each animating position (HH, MM) has a primary
 * label (currently visible) and a secondary label (idle, positioned below
 * the visible area at translate_y=+32, opacity=0). Colon is a single
 * static label between HH and MM. See KTD2/KTD6 in the plan. */
static lv_obj_t *hh_label = NULL;
static lv_obj_t *hh_label_secondary = NULL;
static lv_obj_t *colon_label = NULL;
static lv_obj_t *mm_label = NULL;
static lv_obj_t *mm_label_secondary = NULL;
static lv_obj_t *weather_label = NULL;
static lv_obj_t *temp_label = NULL;
static lv_obj_t *indoor_label = NULL;
static lv_obj_t *station_label = NULL;
static bool s_indicator_on = false;

static const weather_data_t *weather = NULL;
static bool visible = false;
static bool night_mode = false;
static int8_t s_night_override = -1;  /* -1=auto, 0=force day, 1=force night */

/* ── Day-mode slide animation state (U3) ──
 * Each animating position (HH, MM) has one label currently on-screen
 * ("show") and another waiting below ("hide"). trigger_slide() writes
 * the new digits to `hide`, slides `show` up + fades out, slides
 * `hide` up from below + fades in, and the completed_cb swaps the
 * roles so the next call uses the slots in the opposite configuration.
 * Sentinel -1 in last_hh/last_mm signals "first paint — prime only,
 * no animation". reset_animation_state() (called on wake and on
 * day-mode entry) re-aligns roles and clears the sentinel. */
static lv_obj_t *hh_show = NULL;
static lv_obj_t *hh_hide = NULL;
static lv_obj_t *mm_show = NULL;
static lv_obj_t *mm_hide = NULL;
static int last_hh = -1;
static int last_mm = -1;
#define SLIDE_DURATION_MS 300
#define SLIDE_DISTANCE_PX 32

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

/* Forward decl: defined below clock_screen_update_time (used by draw_chart). */
static void set_day_mode_time(int hour, int minute);
static void reset_animation_state(void);
static void trigger_slide(lv_obj_t *show, lv_obj_t *hide, bool do_anim,
                          int new_digits, lv_anim_completed_cb_t completed_cb);

static void draw_chart(void)
{
    if (!canvas || !canvas_buf || !weather || !weather->valid || weather->count < 1) return;
    /* Night mode: canvas shows the dim clock, not the weather chart.
     * Don't overwrite — avoids the 10s flicker (set_indoor_env triggers
     * draw_chart every ~10s but night clock draws every 1s). */
    if (night_mode) return;

    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    /* ── Left: Time ── */
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    set_day_mode_time(tm_now.tm_hour, tm_now.tm_min);
    /* ── Right: Weather (current condition + indoor temp in parens) ── */
    {
        char buf[64];
        const char *indoor = (indoor_label && lv_label_get_text(indoor_label)[0])
                             ? lv_label_get_text(indoor_label) : NULL;
        ESP_LOGI(TAG, "weather row: text='%s' indoor=%s",
                 weather->daily[0].current_text, indoor ? indoor : "(none)");
        if (indoor) {
            snprintf(buf, sizeof(buf), "%s %d\xc2\xb0 %s",
                     weather->daily[0].current_text,
                     weather->daily[0].current, indoor);
        } else {
            snprintf(buf, sizeof(buf), "%s %d\xc2\xb0",
                     weather->daily[0].current_text,
                     weather->daily[0].current);
        }
        lv_label_set_text(weather_label, buf);
    }

    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d\xc2\xb0 - %d\xc2\xb0",
                 weather->daily[0].low, weather->daily[0].high);
        lv_label_set_text(temp_label, buf);
    }

    /* Bottom-left small decorative corner */
    draw_pixel(1, CANVAS_H - 2, COL_SEP);
    draw_pixel(2, CANVAS_H - 2, COL_SEP);
    draw_pixel(1, CANVAS_H - 3, COL_SEP);
}

/* ── 7-segment digit drawing for night mode (1px-wide strokes) ── */

#define NIGHT_COLOR 0x11
#define SEG_W 24   /* digit cell width (multiple of 8: 0..23, right edge at 16) */
#define SEG_H 32   /* digit cell height (multiple of 8: 0..31, bottom at 32) */
#define SEG_GAP 8  /* gap between digits (multiple of 8) */
#define COLON_W 8  /* colon width (multiple of 8) */

static const uint8_t seg7_map[10] = {
    /* 0 */ 0x3F, /* A,B,C,D,E,F */
    /* 1 */ 0x06, /* B,C */
    /* 2 */ 0x5B, /* A,B,G,E,D */
    /* 3 */ 0x4F, /* A,B,G,C,D */
    /* 4 */ 0x66, /* F,G,B,C */
    /* 5 */ 0x6D, /* A,F,G,C,D */
    /* 6 */ 0x7D, /* A,F,G,E,D,C */
    /* 7 */ 0x07, /* A,B,C */
    /* 8 */ 0x7F, /* A,B,C,D,E,F,G */
    /* 9 */ 0x6F, /* A,F,G,B,C,D */
};

/* 8x8 grid mask: keep only top-left pixel in each 8x8 block (1/64 density) */
static void apply_grid_mask(void)
{
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            if ((x & 7) || (y & 7)) {
                canvas_buf[y * CANVAS_W + x] = 0;
            }
        }
    }
}

/* Bit positions: A=0, B=1, C=2, D=3, E=4, F=5, G=6
   All coordinates are multiples of 8 so 8x8 grid mask works uniformly. */
static void draw_seg7_digit(int ox, int oy, int digit, uint8_t gray)
{
    if (digit < 0 || digit > 9) return;
    uint8_t seg = seg7_map[digit];

    /* A: top horizontal y=0, x=0..16 */
    if (seg & 0x01) draw_line(ox + 0, oy + 0, ox + 16, oy + 0, gray);
    /* B: top-right vertical x=16, y=0..16 */
    if (seg & 0x02) draw_line(ox + 16, oy + 0, ox + 16, oy + 16, gray);
    /* C: bottom-right vertical x=16, y=16..32 */
    if (seg & 0x04) draw_line(ox + 16, oy + 16, ox + 16, oy + 32, gray);
    /* D: bottom horizontal y=32, x=0..16 */
    if (seg & 0x08) draw_line(ox + 0, oy + 32, ox + 16, oy + 32, gray);
    /* E: bottom-left vertical x=0, y=16..32 */
    if (seg & 0x10) draw_line(ox + 0, oy + 16, ox + 0, oy + 32, gray);
    /* F: top-left vertical x=0, y=0..16 */
    if (seg & 0x20) draw_line(ox + 0, oy + 0, ox + 0, oy + 16, gray);
    /* G: middle horizontal y=16, x=0..16 */
    if (seg & 0x40) draw_line(ox + 0, oy + 16, ox + 16, oy + 16, gray);
}

static void draw_night_clock(void)
{
    if (!canvas || !canvas_buf) return;

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    int h0 = tm_now.tm_hour / 10;
    int h1 = tm_now.tm_hour % 10;
    int m0 = tm_now.tm_min / 10;
    int m1 = tm_now.tm_min % 10;

    /* Night clock only shows HH:MM — redraw once per minute instead of
     * every second. The per-second full-canvas memset + seg7 draw + grid
     * mask + SPI flush was ~16K CPU iterations and a full 8KB DMA transfer
     * each tick, starving lower-priority I2S DMA and causing audio stutter. */
    static int last_minute = -1;
    static bool last_indicator = false;
    if (tm_now.tm_min == last_minute && s_indicator_on == last_indicator) return;
    last_minute = tm_now.tm_min;
    last_indicator = s_indicator_on;

    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    int total_w = SEG_W * 4 + COLON_W + SEG_GAP * 4;
    int start_x = ((CANVAS_W - total_w) / 2) & ~7; /* force multiple of 8 */
    int start_y = 0; /* multiple of 8 */

    int x = start_x;
    draw_seg7_digit(x, start_y, h0, NIGHT_COLOR); x += SEG_W + SEG_GAP;
    draw_seg7_digit(x, start_y, h1, NIGHT_COLOR); x += SEG_W + SEG_GAP;

    /* Colon: two single dots at multiples of 8 */
    draw_pixel(x + 0, start_y + 16, NIGHT_COLOR);
    draw_pixel(x + 0, start_y + 24, NIGHT_COLOR);
    x += COLON_W + SEG_GAP;

    draw_seg7_digit(x, start_y, m0, NIGHT_COLOR); x += SEG_W + SEG_GAP;
    draw_seg7_digit(x, start_y, m1, NIGHT_COLOR);

    apply_grid_mask();

    /* Audio indicator — drawn after grid mask, top-left 2x2 dot */
    if (s_indicator_on) {
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
                canvas_buf[y * CANVAS_W + x] = NIGHT_COLOR;
    }

    lv_obj_invalidate(canvas);
}

/* Update the day-mode time labels for the given hour and minute.
 * Writes only to the primary HH/MM labels — the colon is static,
 * and the secondary labels are managed by the animation trigger path. */
static void set_day_mode_time(int hour, int minute)
{
    char hh_buf[3], mm_buf[3];
    snprintf(hh_buf, sizeof(hh_buf), "%02d", (unsigned)hour);
    snprintf(mm_buf, sizeof(mm_buf), "%02d", (unsigned)minute);
    lv_label_set_text(hh_label, hh_buf);
    lv_label_set_text(mm_label, mm_buf);
}

/* LVGL animation callbacks: translate_y and opacity are style properties,
 * so we wrap the style setters to fit the lv_anim_exec_xcb_t signature
 * (void (*)(void *var, int32_t value)). */
static void anim_exec_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)value, 0);
}

static void anim_exec_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/* Called from clock_screen_show() and the day-mode entry branch of
 * clock_screen_set_night_mode(). Re-aligns show/hide roles to their
 * create-time defaults, cancels any in-flight animations on the four
 * time labels, and primes last_hh/last_mm back to the sentinel so the
 * next clock_screen_update_time() tick is treated as a fresh paint
 * (R5/R6: no animation on first paint after wake or mode switch). */
static void reset_animation_state(void)
{
    last_hh = -1;
    last_mm = -1;

    /* Cancel any in-flight animations on each of the four time labels. */
    lv_anim_delete(hh_label, anim_exec_translate_y);
    lv_anim_delete(hh_label, anim_exec_opa);
    lv_anim_delete(hh_label_secondary, anim_exec_translate_y);
    lv_anim_delete(hh_label_secondary, anim_exec_opa);
    lv_anim_delete(mm_label, anim_exec_translate_y);
    lv_anim_delete(mm_label, anim_exec_opa);
    lv_anim_delete(mm_label_secondary, anim_exec_translate_y);
    lv_anim_delete(mm_label_secondary, anim_exec_opa);

    /* Restore create-time positions and visibility. */
    if (hh_label) {
        lv_obj_set_style_translate_y(hh_label, 0, 0);
        lv_obj_set_style_opa(hh_label, LV_OPA_COVER, 0);
    }
    if (hh_label_secondary) {
        lv_obj_set_style_translate_y(hh_label_secondary, SLIDE_DISTANCE_PX, 0);
        lv_obj_set_style_opa(hh_label_secondary, LV_OPA_TRANSP, 0);
        lv_label_set_text(hh_label_secondary, "");
    }
    if (mm_label) {
        lv_obj_set_style_translate_y(mm_label, 0, 0);
        lv_obj_set_style_opa(mm_label, LV_OPA_COVER, 0);
    }
    if (mm_label_secondary) {
        lv_obj_set_style_translate_y(mm_label_secondary, SLIDE_DISTANCE_PX, 0);
        lv_obj_set_style_opa(mm_label_secondary, LV_OPA_TRANSP, 0);
        lv_label_set_text(mm_label_secondary, "");
    }

    hh_show = hh_label;
    hh_hide = hh_label_secondary;
    mm_show = mm_label;
    mm_hide = mm_label_secondary;
}

static void hh_anim_completed_cb(lv_anim_t *a)
{
    /* Swap roles: the label that just slid in becomes the new "show",
     * the one that slid out becomes the new "hide" waiting below. */
    lv_obj_t *tmp = hh_show;
    hh_show = hh_hide;
    hh_hide = tmp;
    /* Park the now-out-of-view "hide" off-screen for the next cycle. */
    lv_obj_set_style_translate_y(hh_hide, SLIDE_DISTANCE_PX, 0);
    lv_obj_set_style_opa(hh_hide, LV_OPA_TRANSP, 0);
    lv_label_set_text(hh_hide, "");
    /* The user_data carries the value that was being animated to; update
     * last_hh so subsequent ticks can detect the next transition. */
    last_hh = (int)(intptr_t)lv_anim_get_user_data(a);
}

static void mm_anim_completed_cb(lv_anim_t *a)
{
    lv_obj_t *tmp = mm_show;
    mm_show = mm_hide;
    mm_hide = tmp;
    lv_obj_set_style_translate_y(mm_hide, SLIDE_DISTANCE_PX, 0);
    lv_obj_set_style_opa(mm_hide, LV_OPA_TRANSP, 0);
    lv_label_set_text(mm_hide, "");
    last_mm = (int)(intptr_t)lv_anim_get_user_data(a);
}

/* Slide the digit pair: existing on-screen digits (`show`) move up + out
 * while the new digits (`hide`) slide in from below. Called per position
 * (HH and MM separately); both fire when the hour changes (and the minute
 * necessarily changes with it). */
static void trigger_slide(lv_obj_t *show, lv_obj_t *hide, bool do_anim,
                          int new_digits, lv_anim_completed_cb_t completed_cb)
{
    if (!do_anim) return;

    char buf[3];
    snprintf(buf, sizeof(buf), "%02d", (unsigned)new_digits);
    lv_label_set_text(hide, buf);

    /* Re-anchor `hide` below the visible area and fully transparent so
     * it starts the animation cleanly even if a prior cycle left it
     * offset. */
    lv_obj_set_style_translate_y(hide, SLIDE_DISTANCE_PX, 0);
    lv_obj_set_style_opa(hide, LV_OPA_TRANSP, 0);

    /* Animation 1: show slides up 0 → -32 */
    lv_anim_t a1;
    lv_anim_init(&a1);
    lv_anim_set_var(&a1, show);
    lv_anim_set_duration(&a1, SLIDE_DURATION_MS);
    lv_anim_set_path_cb(&a1, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a1, anim_exec_translate_y);
    lv_anim_set_values(&a1, 0, -SLIDE_DISTANCE_PX);
    lv_anim_start(&a1);

    /* Animation 2: hide slides up +32 → 0 */
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, hide);
    lv_anim_set_duration(&a2, SLIDE_DURATION_MS);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a2, anim_exec_translate_y);
    lv_anim_set_values(&a2, SLIDE_DISTANCE_PX, 0);
    lv_anim_start(&a2);

    /* Animation 3: show fades out (255 → 0) */
    lv_anim_t a3;
    lv_anim_init(&a3);
    lv_anim_set_var(&a3, show);
    lv_anim_set_duration(&a3, SLIDE_DURATION_MS);
    lv_anim_set_path_cb(&a3, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a3, anim_exec_opa);
    lv_anim_set_values(&a3, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_start(&a3);

    /* Animation 4: hide fades in (0 → 255). Hook the role-swap
     * completed_cb here — same duration as the others, so all four
     * finish within the same LVGL tick. */
    lv_anim_t a4;
    lv_anim_init(&a4);
    lv_anim_set_var(&a4, hide);
    lv_anim_set_duration(&a4, SLIDE_DURATION_MS);
    lv_anim_set_path_cb(&a4, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a4, anim_exec_opa);
    lv_anim_set_values(&a4, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_completed_cb(&a4, completed_cb);
    lv_anim_set_user_data(&a4, (void *)(intptr_t)new_digits);
    lv_anim_start(&a4);
}

void clock_screen_update_time(void)
{
    if (!visible || !hh_show) return;

    if (night_mode) {
        draw_night_clock();
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    int cur_hh = tm_now.tm_hour;
    int cur_mm = tm_now.tm_min;

    /* First paint (sentinel still -1): prime the labels with current time,
     * skip animation. See reset_animation_state() callers in
     * clock_screen_show() and the day-mode branch of
     * clock_screen_set_night_mode(). */
    if (last_hh == -1) {
        set_day_mode_time(cur_hh, cur_mm);
        last_hh = cur_hh;
        last_mm = cur_mm;
        return;
    }

    /* No digit change: nothing to animate (R8 — per-second tick stays cheap). */
    if (cur_hh == last_hh && cur_mm == last_mm) return;

    bool hh_changed = (cur_hh != last_hh);
    bool mm_changed = (cur_mm != last_mm);
    trigger_slide(hh_show, hh_hide, hh_changed, cur_hh, hh_anim_completed_cb);
    trigger_slide(mm_show, mm_hide, mm_changed, cur_mm, mm_anim_completed_cb);
}

/* ── Public API ── */

lv_obj_t *clock_screen_create(lv_obj_t *parent)
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

    /* ── Time labels (left half, big digital-7) ──
     * Five labels total: HH primary, HH secondary, colon (static),
     * MM primary, MM secondary. Layout per KTD6: HH at x=0..44, colon
     * at x=44..66, MM at x=66..110 within the 128px time area at y=2.
     * Secondary labels are positioned below the visible area
     * (translate_y=+32, opacity 0) and slide in for minute transitions. */
    hh_label = lv_label_create(container);
    lv_obj_set_style_text_color(hh_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(hh_label, &lv_font_digital, 0);
    lv_obj_set_style_text_align(hh_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hh_label, 0, 2);
    lv_obj_set_size(hh_label, 44, 48);
    lv_label_set_text(hh_label, "");

    hh_label_secondary = lv_label_create(container);
    lv_obj_set_style_text_color(hh_label_secondary, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(hh_label_secondary, &lv_font_digital, 0);
    lv_obj_set_style_text_align(hh_label_secondary, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hh_label_secondary, 0, 2);
    lv_obj_set_size(hh_label_secondary, 44, 48);
    lv_obj_set_style_translate_y(hh_label_secondary, 32, 0);
    lv_obj_set_style_opa(hh_label_secondary, LV_OPA_TRANSP, 0);
    lv_label_set_text(hh_label_secondary, "");

    colon_label = lv_label_create(container);
    lv_obj_set_style_text_color(colon_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(colon_label, &lv_font_digital, 0);
    lv_obj_set_style_text_align(colon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(colon_label, 44, 2);
    lv_obj_set_size(colon_label, 22, 48);
    lv_label_set_text(colon_label, ":");

    mm_label = lv_label_create(container);
    lv_obj_set_style_text_color(mm_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(mm_label, &lv_font_digital, 0);
    lv_obj_set_style_text_align(mm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mm_label, 66, 2);
    lv_obj_set_size(mm_label, 44, 48);
    lv_label_set_text(mm_label, "");

    mm_label_secondary = lv_label_create(container);
    lv_obj_set_style_text_color(mm_label_secondary, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(mm_label_secondary, &lv_font_digital, 0);
    lv_obj_set_style_text_align(mm_label_secondary, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mm_label_secondary, 66, 2);
    lv_obj_set_size(mm_label_secondary, 44, 48);
    lv_obj_set_style_translate_y(mm_label_secondary, 32, 0);
    lv_obj_set_style_opa(mm_label_secondary, LV_OPA_TRANSP, 0);
    lv_label_set_text(mm_label_secondary, "");

    /* Initialize animation roles. reset_animation_state() re-aligns these
     * on every wake-up and day-mode entry; this is the create-time
     * baseline so the first clock_screen_update_time() tick can find
     * hh_show/mm_show before reset_animation_state() runs (e.g., if the
     * 1Hz tick fires before clock_screen_show() is called). */
    hh_show = hh_label;
    hh_hide = hh_label_secondary;
    mm_show = mm_label;
    mm_hide = mm_label_secondary;

    /* ── Indoor temp cache (hidden label, used by draw_chart) ── */
    indoor_label = lv_label_create(container);
    lv_label_set_text(indoor_label, "");
    lv_obj_add_flag(indoor_label, LV_OBJ_FLAG_HIDDEN);

    /* ── Weather text (right half, top row — with indoor in parens) ── */
    weather_label = lv_label_create(container);
    lv_obj_set_style_text_color(weather_label, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_font(weather_label, &lv_font_station, 0);
    lv_obj_set_style_text_align(weather_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(weather_label, SEP_X + 7, 0);
    lv_obj_set_size(weather_label, CANVAS_W - SEP_X - 9, 18);
    lv_label_set_long_mode(weather_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(weather_label, "");
    lv_obj_add_flag(weather_label, LV_OBJ_FLAG_HIDDEN);

    /* ── Temperature range label (right half, second row) ── */
    temp_label = lv_label_create(container);
    lv_obj_set_style_text_color(temp_label, lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
    lv_obj_set_style_text_font(temp_label, &lv_font_station, 0);
    lv_obj_set_style_text_align(temp_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(temp_label, SEP_X + 7, 18);
    lv_obj_set_size(temp_label, CANVAS_W - SEP_X - 9, 18);
    lv_label_set_long_mode(temp_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(temp_label, "");
    lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);

    /* ── Station name (bottom row, full width, centered) ── */
    station_label = lv_label_create(container);
    lv_obj_set_style_text_color(station_label, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_font(station_label, &lv_font_station, 0);
    lv_obj_set_style_text_align(station_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(station_label, 0, 36);
    lv_obj_set_size(station_label, CANVAS_W, 18);
    lv_obj_set_style_bg_opa(station_label, LV_OPA_TRANSP, 0);
    lv_label_set_long_mode(station_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(station_label, "");
    lv_obj_add_flag(station_label, LV_OBJ_FLAG_HIDDEN);

    return container;
}

void clock_screen_set_station_name(const char *name)
{
    if (!station_label) return;
    if (night_mode) return;

    if (name && name[0]) {
        lv_label_set_text(station_label, name);
    } else {
        lv_label_set_text(station_label, "Streaming...");
    }
    lv_obj_clear_flag(station_label, LV_OBJ_FLAG_HIDDEN);
}

void clock_screen_set_audio_indicator(bool on)
{
    s_indicator_on = on;
    if (canvas) lv_obj_invalidate(canvas);
}

void clock_screen_set_indoor_env(float temp_c, float humidity)
{
    if (!indoor_label) return;

    if (isnan(temp_c)) {  /* sensor absent */
        ESP_LOGI(TAG, "indoor env: sensor absent (NaN)");
        lv_label_set_text(indoor_label, "");
        if (weather && weather->valid) draw_chart();
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "(内%.1f\xc2\xb0%.0f%%)", temp_c, humidity);
    ESP_LOGI(TAG, "indoor env: %.1f°C %.0f%%RH  →  suffix='%s'", temp_c, humidity, buf);
    lv_label_set_text(indoor_label, buf);
    if (weather && weather->valid) draw_chart();
}

void clock_screen_set_indoor_full(float temp_c, float humidity)
{
    if (!weather_label) return;
    if (night_mode) return;

    char buf[32];
    if (isnan(temp_c)) {
        lv_label_set_text(weather_label, "");
    } else {
        snprintf(buf, sizeof(buf), "室内 %.1f\xc2\xb0 %.0f%%", temp_c, humidity);
        lv_label_set_text(weather_label, buf);
    }
    lv_obj_remove_flag(weather_label, LV_OBJ_FLAG_HIDDEN);
}

void clock_screen_set_alarm_time(int hour, int minute)
{
    if (!temp_label) return;
    if (night_mode) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "闹钟 %02d:%02d", hour, minute);
    lv_label_set_text(temp_label, buf);
    lv_obj_remove_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
}

void clock_screen_show_button_hint(void)
{
    if (!station_label) return;
    if (night_mode) return;

    lv_label_set_text(station_label, "按左键播放");
    lv_obj_clear_flag(station_label, LV_OBJ_FLAG_HIDDEN);
}

void clock_screen_show_button_hint_agent_off(void)
{
    if (!station_label) return;
    if (night_mode) return;

    lv_label_set_text(station_label, "左键同步时间 · 右键睡眠");
    lv_obj_clear_flag(station_label, LV_OBJ_FLAG_HIDDEN);
}

void clock_screen_set_data(const weather_data_t *data)
{
    weather = data;
    if (!weather || !weather->valid) {
        ESP_LOGI(TAG, "set_data: invalid weather data");
        return;
    }
    ESP_LOGI(TAG, "set_data: %s %d°/%d°",
             weather->daily[0].day_text,
             weather->daily[0].high, weather->daily[0].low);

    if (night_mode) return; /* Don't change visibility during night mode */

    /* Show weather data */
    if (weather_label) lv_obj_remove_flag(weather_label, LV_OBJ_FLAG_HIDDEN);
    if (temp_label) lv_obj_remove_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    draw_chart();
    lv_obj_invalidate(container);
    lv_refr_now(lv_disp_get_default());
}

void clock_screen_show(void)
{
    if (!container) return;
    visible = true;

    /* Prime the slide animation state so the first paint after wake-up
     * is static (R5). reset_animation_state() also clears any in-flight
     * animations left over from the previous active session. */
    reset_animation_state();

    /* Apply night/day mode based on current time on first show.
     * night_mode starts as false but the clock may already be in night
     * hours (e.g. after a cold boot at 22:00). Without this, the
     * display shows a broken half-night-half-day state until the user
     * long-presses the button. */
    clock_screen_set_night_mode(clock_screen_is_night_time());

    if (weather && weather->valid) {
        draw_chart();
    }
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(container);
}

void clock_screen_hide(void)
{
    if (!container) return;
    visible = false;
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

bool clock_screen_is_visible(void)
{
    return visible;
}

bool clock_screen_is_night_time(void)
{
    /* Override takes priority (session-only, resets to auto on wake) */
    if (s_night_override == 1) return true;   /* force night */
    if (s_night_override == 0) return false;  /* force day */

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    int h = tm_now.tm_hour;
    int m = tm_now.tm_min;
    int start_h = CONFIG_NIGHT_START_HOUR;
    int start_m = CONFIG_NIGHT_START_MINUTE;
    int end_h = CONFIG_NIGHT_END_HOUR;

    bool after_start = (h > start_h) || (h == start_h && m >= start_m);
    bool before_end = (h < end_h);

    if (start_h > end_h) {
        /* Night crosses midnight (e.g. 22:00 → 06:00) */
        return after_start || before_end;
    } else {
        /* Night within a single day (e.g. 03:00 → 06:00) */
        return after_start && before_end;
    }
}

void clock_screen_set_night_override(int8_t override)
{
    s_night_override = override;
}

int8_t clock_screen_get_night_override(void)
{
    return s_night_override;
}

void clock_screen_set_night_mode(bool enable)
{
    if (night_mode == enable) return;
    night_mode = enable;

    if (enable) {
        ssd1322_set_contrast(0x01); /* dim contrast current */
        if (canvas) lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        if (hh_label) lv_obj_add_flag(hh_label, LV_OBJ_FLAG_HIDDEN);
        if (hh_label_secondary) lv_obj_add_flag(hh_label_secondary, LV_OBJ_FLAG_HIDDEN);
        if (colon_label) lv_obj_add_flag(colon_label, LV_OBJ_FLAG_HIDDEN);
        if (mm_label) lv_obj_add_flag(mm_label, LV_OBJ_FLAG_HIDDEN);
        if (mm_label_secondary) lv_obj_add_flag(mm_label_secondary, LV_OBJ_FLAG_HIDDEN);
        if (weather_label) lv_obj_add_flag(weather_label, LV_OBJ_FLAG_HIDDEN);
        if (temp_label) lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
        if (indoor_label) lv_obj_add_flag(indoor_label, LV_OBJ_FLAG_HIDDEN);
        if (station_label) lv_obj_add_flag(station_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        ssd1322_set_contrast(0x9F); /* restore normal contrast */

        /* Reset the slide animation state on every day-mode entry (R6:
         * first paint after night→day switch is static, no animation).
         * set_night_mode(true) does not need this — labels are hidden
         * anyway and reset_animation_state() runs on the next day entry. */
        reset_animation_state();

        if (canvas) lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        if (hh_label) lv_obj_remove_flag(hh_label, LV_OBJ_FLAG_HIDDEN);
        if (colon_label) lv_obj_remove_flag(colon_label, LV_OBJ_FLAG_HIDDEN);
        if (mm_label) lv_obj_remove_flag(mm_label, LV_OBJ_FLAG_HIDDEN);
        /* hh_label_secondary and mm_label_secondary stay hidden until
         * U3's transition-detection path triggers their first slide-in. */
        if (weather && weather->valid) {
            if (weather_label) lv_obj_remove_flag(weather_label, LV_OBJ_FLAG_HIDDEN);
            if (temp_label) lv_obj_remove_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (station_label && lv_label_get_text(station_label)[0]) {
            lv_obj_remove_flag(station_label, LV_OBJ_FLAG_HIDDEN);
        }
        draw_chart();
    }
    lv_obj_invalidate(container);
}

void clock_screen_deinit(void)
{
    if (canvas_buf) {
        free(canvas_buf);
        canvas_buf = NULL;
    }
}
