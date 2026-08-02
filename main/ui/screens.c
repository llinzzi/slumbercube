#include "screens.h"

#define SPRITE_W 72
#define SPRITE_H 64

static lv_obj_t *s_screen;
static lv_obj_t *s_ghast;
LV_DRAW_BUF_DEFINE_STATIC(s_ghast_buf, SPRITE_W, SPRITE_H, LV_COLOR_FORMAT_RGB565);

static bool s_eyes_closed;
static uint8_t s_blink_step;
static uint8_t s_float_step;
static int16_t s_patrol_x = -64;
static int8_t s_direction = 1;
static uint8_t s_turn_step;
static bool s_turning;

static lv_color_t gray(uint8_t value)
{
    return lv_color_make(value, value, value);
}

/* Draw logical pixel-art rectangles into one canvas.  Keeping the entire
 * character in a single LVGL object avoids the deep draw tree that previously
 * overflowed app_main's stack during the first refresh. */
static void block(int x, int y, int w, int h, uint8_t value,
                  int scale, bool mirror)
{
    const int center = SPRITE_W / 2;
    lv_color_t color = gray(value);

    for(int py = y; py < y + h; py++) {
        if(py < 0 || py >= SPRITE_H) continue;
        for(int px = x; px < x + w; px++) {
            int logical_x = mirror ? (SPRITE_W - 1 - px) : px;
            int dx = center + ((logical_x - center) * scale) / 64;
            int dx_next = center + ((logical_x + 1 - center) * scale) / 64;
            if(dx_next <= dx) dx_next = dx + 1;
            for(int out_x = dx; out_x < dx_next; out_x++) {
                if(out_x >= 0 && out_x < SPRITE_W) {
                    lv_canvas_set_px(s_ghast, out_x, py, color, LV_OPA_COVER);
                }
            }
        }
    }
}

static void draw_ghast(void)
{
    /* Turn through the front, not around the back.  The head becomes slightly
     * wider at the centered front pose, then settles into the opposite 3/4. */
    static const uint8_t turn_width[] = { 64, 66, 68, 70, 72, 70, 68, 66, 64 };
    int scale = s_turning ? turn_width[s_turn_step] : 64;
    /* The visible side plane must lead the motion: right-facing while moving
     * right, and the mirrored left-facing pose while moving left. */
    bool mirror = s_direction > 0;
    if(s_turning && s_turn_step >= 5) mirror = !mirror;
    int yaw = s_turning ? s_turn_step : 0;

    lv_canvas_fill_bg(s_ghast, gray(0x08), LV_OPA_COVER);

    /* Three value-separated planes make the 40px head read as a cube. */
    block(18, 1, 38, 2, 0xf8, scale, mirror);
    block(15, 3, 41, 2, 0xe8, scale, mirror);
    block(12, 5, 44, 2, 0xdc, scale, mirror);
    block(9, 7, 47, 2, 0xd0, scale, mirror);
    block(8, 9, 40, 38, 0xc4, scale, mirror);
    block(48, 9, 12, 31, 0x68, scale, mirror);
    block(48, 40, 9, 3, 0x60, scale, mirror);
    block(48, 43, 6, 2, 0x58, scale, mirror);
    block(48, 45, 3, 2, 0x50, scale, mirror);

    /* The dark side plane collapses into the front at mid-turn.  Covering it
     * with the front value on frames 2..6 makes the lighting rotate toward
     * the viewer instead of exposing a rear face. */
    if(s_turning && yaw >= 2 && yaw <= 6) {
        uint8_t front_value = yaw == 4 ? 0xcc : 0xc8;
        block(45, 9, 15, 38, front_value, scale, mirror);
        if(yaw != 4) block(47, 12, 3, 31, 0xa0, scale, mirror);
        block(18, 5, 30, 2, yaw == 4 ? 0xf4 : 0xe4, scale, mirror);
    }

    /* Mottled GBA-style grayscale texture. */
    block(11, 12, 7, 5, 0x98, scale, mirror);
    block(38, 13, 7, 4, 0xe0, scale, mirror);
    block(13, 38, 8, 6, 0x9c, scale, mirror);
    block(39, 35, 6, 7, 0x90, scale, mirror);
    block(51, 14, 6, 6, 0x48, scale, mirror);
    block(51, 31, 8, 6, 0x50, scale, mirror);
    block(25, 3, 9, 2, 0xff, scale, mirror);

    /* Keep the face visible: this turn deliberately passes through a frontal
     * pose.  Hiding it here makes the character appear to turn backward. */
    {
        int face_shift = 0;
        if(s_turning && yaw < 4) face_shift = -(4 - yaw) / 2;
        if(s_turning && yaw > 4) face_shift = (yaw - 4) / 2;
        if(s_eyes_closed) {
            block(16 + face_shift, 26, 7, 2, 0x18, scale, mirror);
            block(34 + face_shift, 26, 7, 2, 0x18, scale, mirror);
        }
        else {
            /* Clean 2-tone sockets: black upper eye, one restrained gray tear
             * underneath.  Large nested rectangles looked noisy at 256x64. */
            block(16 + face_shift, 22, 7, 5, 0x10, scale, mirror);
            block(34 + face_shift, 22, 7, 5, 0x10, scale, mirror);
            block(18 + face_shift, 27, 4, 4, 0x60, scale, mirror);
            block(35 + face_shift, 27, 4, 4, 0x60, scale, mirror);
            block(19 + face_shift, 23, 2, 2, 0x02, scale, mirror);
            block(35 + face_shift, 23, 2, 2, 0x02, scale, mirror);
        }

        /* Small downturned mouth: a simple dark bar with lowered corners is
         * much closer to the Ghast's mournful texture than an open black box. */
        block(24 + face_shift, 36, 10, 3, 0x20, scale, mirror);
        block(24 + face_shift, 38, 3, 3, 0x20, scale, mirror);
        block(31 + face_shift, 38, 3, 3, 0x20, scale, mirror);
        block(27 + face_shift, 37, 4, 2, 0x04, scale, mirror);
    }

    /* Five uneven tentacles lag in alternating phases while the body floats. */
    static const int x[] = { 11, 20, 29, 38, 49 };
    static const int h[] = { 11, 15, 12, 16, 10 };
    for(int i = 0; i < 5; i++) {
        int sway = ((s_float_step + i * 2) % 8 >= 4) ? 2 : 0;
        uint8_t shade = i == 4 ? 0x68 : (i & 1 ? 0x98 : 0xac);
        block(x[i], 47 + sway, 5, h[i] - sway, shade, scale, mirror);
        block(x[i], 47 + h[i] - 3, 5, 3, i == 4 ? 0x48 : 0x70,
              scale, mirror);
    }

    lv_obj_invalidate(s_ghast);
}

static void blink_cb(lv_timer_t *timer)
{
    static const uint32_t open_times[] = { 2600, 3900, 1800, 500, 3200 };
    s_eyes_closed = !s_eyes_closed;
    draw_ghast();

    if(s_eyes_closed) {
        lv_timer_set_period(timer, 180);
    }
    else {
        lv_timer_set_period(timer, open_times[s_blink_step]);
        s_blink_step = (s_blink_step + 1) % 5;
    }
}

static void motion_cb(lv_timer_t *timer)
{
    static const int8_t y_path[] = { 0, -1, -2, -2, -1, 0, 2, 3, 4, 4, 3, 2 };

    if(s_turning) {
        if(s_turn_step < 8) {
            s_turn_step++;
        }
        else {
            s_direction = -s_direction;
            s_turning = false;
            s_turn_step = 0;
        }
    }
    else {
        s_patrol_x += s_direction * 3;
        /* Turn while the complete body is visible, rather than disappearing
         * beyond the bezel and silently changing direction off-screen. */
        if((s_direction > 0 && s_patrol_x >= 190) ||
           (s_direction < 0 && s_patrol_x <= -2)) {
            s_turning = true;
            s_turn_step = 0;
        }
    }

    lv_obj_set_pos(s_ghast, s_patrol_x, y_path[s_float_step]);
    s_float_step = (s_float_step + 1) % 12;
    draw_ghast();
    lv_timer_set_period(timer, s_turning ? 90 : 120);
}

void screens_set_weather_data_ptr(const void *data)
{
    (void)data;
}

void create_screen_main(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 256, 64);
    lv_obj_set_style_bg_color(s_screen, gray(0x08), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    LV_DRAW_BUF_INIT_STATIC(s_ghast_buf);
    s_ghast = lv_canvas_create(s_screen);
    lv_canvas_set_draw_buf(s_ghast, &s_ghast_buf);
    lv_obj_set_pos(s_ghast, s_patrol_x, 0);
    lv_obj_clear_flag(s_ghast, LV_OBJ_FLAG_SCROLLABLE);
    draw_ghast();

    lv_screen_load(s_screen);
    lv_timer_create(blink_cb, 1800, NULL);
    lv_timer_create(motion_cb, 120, NULL);
}

void tick_screen_main(void)
{
    /* Motion is driven by LVGL timers. */
}

void create_screens(void)
{
    create_screen_main();
}
