#include "screens.h"

static lv_obj_t *s_screen;
static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static lv_obj_t *s_left_pupil;
static lv_obj_t *s_right_pupil;
static lv_obj_t *s_left_ear;
static lv_obj_t *s_right_ear;
static lv_obj_t *s_wave_paw;
static uint8_t s_blink_step;
static uint8_t s_gaze_step;
static uint8_t s_action_step;
static bool s_blink_closed;

static lv_obj_t *pixel(lv_obj_t *parent, int32_t x, int32_t y,
                       int32_t w, int32_t h, uint8_t gray)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_make(gray, gray, gray), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *transparent_group(lv_obj_t *parent, int32_t x, int32_t y,
                                   int32_t w, int32_t h)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_set_size(group, w, h);
    lv_obj_set_pos(group, x, y);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    return group;
}

static lv_obj_t *create_ear(int32_t x)
{
    lv_obj_t *ear = transparent_group(s_screen, x, 4, 20, 16);
    pixel(ear, 0, 4, 20, 12, 0x58);
    pixel(ear, 4, 0, 12, 8, 0x58);
    pixel(ear, 5, 4, 10, 8, 0xA0);
    return ear;
}

static lv_obj_t *create_eye(int32_t x, lv_obj_t **pupil_out)
{
    lv_obj_t *eye = pixel(s_screen, x, 24, 28, 18, 0xD8);
    pixel(eye, 0, 0, 4, 4, 0x88);
    pixel(eye, 24, 0, 4, 4, 0x88);

    lv_obj_t *pupil = pixel(eye, 12, 4, 8, 14, 0x18);
    pixel(pupil, 0, 0, 4, 4, 0xFF);
    *pupil_out = pupil;
    return eye;
}

static lv_obj_t *create_paw(int32_t x)
{
    lv_obj_t *paw = transparent_group(s_screen, x, 48, 40, 16);
    pixel(paw, 0, 0, 40, 16, 0xC0);
    pixel(paw, 0, 0, 4, 4, 0x78);
    pixel(paw, 36, 0, 4, 4, 0x78);
    pixel(paw, 8, 5, 4, 11, 0x68);
    pixel(paw, 20, 5, 4, 11, 0x68);
    pixel(paw, 32, 5, 4, 11, 0x68);
    return paw;
}

static void set_eye_height(void *obj, int32_t h)
{
    lv_obj_t *eye = obj;
    lv_obj_set_height(eye, h);
    lv_obj_set_y(eye, 24 + (18 - h) / 2);
}

static void animate_blink(lv_obj_t *eye)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye);
    lv_anim_set_exec_cb(&a, set_eye_height);
    lv_anim_set_values(&a, 18, 2);
    lv_anim_set_duration(&a, 75);
    lv_anim_set_playback_duration(&a, 125);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void blink_cb(lv_timer_t *timer)
{
    static const uint32_t rhythm[] = { 2400, 3400, 1800, 380, 2900 };
    s_blink_closed = !s_blink_closed;
    set_eye_height(s_left_eye, s_blink_closed ? 2 : 18);
    set_eye_height(s_right_eye, s_blink_closed ? 2 : 18);
    if(s_blink_closed) {
        lv_timer_set_period(timer, 260);
    }
    else {
        lv_timer_set_period(timer, rhythm[s_blink_step]);
        s_blink_step = (s_blink_step + 1) % 5;
    }
}

static void set_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void move_x(lv_obj_t *obj, int32_t target)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_x);
    lv_anim_set_values(&a, lv_obj_get_x(obj), target);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void gaze_cb(lv_timer_t *timer)
{
    static const int32_t positions[] = { 16, 12, 6, 12 };
    set_x(s_left_pupil, positions[s_gaze_step]);
    set_x(s_right_pupil, positions[s_gaze_step]);
    s_gaze_step = (s_gaze_step + 1) % 4;
    lv_timer_set_period(timer, s_gaze_step == 0 ? 2200 : 1300);
}

static void set_y(void *obj, int32_t y)
{
    lv_obj_set_y(obj, y);
}

static void bounce_y(lv_obj_t *obj, int32_t rest_y, int32_t raised_y,
                     uint32_t up_ms, uint32_t down_ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_y);
    lv_anim_set_values(&a, rest_y, raised_y);
    lv_anim_set_duration(&a, up_ms);
    lv_anim_set_playback_duration(&a, down_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void action_cb(lv_timer_t *timer)
{
    if(s_action_step == 0) {
        set_y(s_left_ear, 0);
        lv_timer_set_period(timer, 400);
    }
    else if(s_action_step == 1) {
        set_y(s_left_ear, 4);
        set_y(s_right_ear, 0);
        lv_timer_set_period(timer, 400);
    }
    else if(s_action_step == 2) {
        set_y(s_right_ear, 4);
        set_y(s_wave_paw, 32);
        lv_timer_set_period(timer, 700);
    }
    else {
        set_y(s_wave_paw, 48);
        lv_timer_set_period(timer, 2200);
    }
    s_action_step = (s_action_step + 1) % 4;
}

void screens_set_weather_data_ptr(const void *data)
{
    (void)data;
}

void create_screen_main(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 256, 64);
    lv_obj_set_style_bg_color(s_screen, lv_color_make(0x08, 0x08, 0x08), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_left_ear = create_ear(64);
    s_right_ear = create_ear(176);
    pixel(s_screen, 64, 16, 128, 32, 0x58);
    pixel(s_screen, 72, 12, 112, 40, 0x68);

    s_left_eye = create_eye(84, &s_left_pupil);
    s_right_eye = create_eye(144, &s_right_pupil);

    pixel(s_screen, 112, 38, 32, 12, 0x98);
    pixel(s_screen, 124, 39, 8, 4, 0x20);
    pixel(s_screen, 120, 43, 4, 4, 0x38);
    pixel(s_screen, 132, 43, 4, 4, 0x38);
    pixel(s_screen, 52, 40, 20, 2, 0x70);
    pixel(s_screen, 184, 40, 20, 2, 0x70);
    pixel(s_screen, 56, 45, 16, 2, 0x48);
    pixel(s_screen, 184, 45, 16, 2, 0x48);

    create_paw(72);
    s_wave_paw = create_paw(148);

    lv_screen_load(s_screen);
    lv_timer_create(blink_cb, 1700, NULL);
    lv_timer_create(gaze_cb, 900, NULL);
    lv_timer_create(action_cb, 1500, NULL);
}

void tick_screen_main(void)
{
    /* The cat is driven entirely by LVGL timers and animations. */
}

void create_screens(void)
{
    create_screen_main();
}
