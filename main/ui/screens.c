#include "screens.h"

static lv_obj_t *s_main_screen = NULL;
static lv_obj_t *s_left_eye = NULL;
static lv_obj_t *s_right_eye = NULL;
static lv_obj_t *s_left_pupil = NULL;
static lv_obj_t *s_right_pupil = NULL;
static uint8_t s_blink_count = 0;
static uint8_t s_gaze_step = 0;

static void set_eye_opening(void *obj, int32_t height)
{
    lv_obj_t *eye = obj;

    lv_obj_set_height(eye, height);
    lv_obj_set_y(eye, 12 + (40 - height) / 2);
}

static lv_obj_t *create_eye(lv_obj_t *parent, int32_t x)
{
    lv_obj_t *eye = lv_obj_create(parent);

    lv_obj_set_size(eye, 40, 40);
    lv_obj_set_pos(eye, x, 12);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_make(0xD8, 0xD8, 0xD8), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(eye, lv_color_make(0x68, 0x68, 0x68), 0);
    lv_obj_set_style_border_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye, 2, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);

    return eye;
}

static lv_obj_t *create_pupil(lv_obj_t *eye)
{
    lv_obj_t *iris = lv_obj_create(eye);

    lv_obj_set_size(iris, 20, 24);
    lv_obj_set_pos(iris, 10, 8);
    lv_obj_set_style_radius(iris, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(iris, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_set_style_bg_opa(iris, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(iris, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_set_style_border_opa(iris, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(iris, 1, 0);
    lv_obj_set_style_pad_all(iris, 0, 0);
    lv_obj_clear_flag(iris, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pupil = lv_obj_create(iris);
    lv_obj_set_size(pupil, 9, 14);
    lv_obj_set_pos(pupil, 5, 5);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil, lv_color_make(0x10, 0x10, 0x10), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pupil, 0, 0);
    lv_obj_set_style_pad_all(pupil, 0, 0);
    lv_obj_clear_flag(pupil, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *highlight = lv_obj_create(iris);
    lv_obj_set_size(highlight, 5, 6);
    lv_obj_set_pos(highlight, 4, 3);
    lv_obj_set_style_radius(highlight, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(highlight, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(highlight, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(highlight, 0, 0);
    lv_obj_set_style_pad_all(highlight, 0, 0);
    lv_obj_clear_flag(highlight, LV_OBJ_FLAG_SCROLLABLE);

    return iris;
}

static void start_blink(lv_obj_t *eye)
{
    lv_anim_t blink;

    lv_anim_init(&blink);
    lv_anim_set_var(&blink, eye);
    lv_anim_set_exec_cb(&blink, set_eye_opening);
    lv_anim_set_values(&blink, 40, 2);
    lv_anim_set_duration(&blink, 85);
    lv_anim_set_playback_duration(&blink, 135);
    lv_anim_set_path_cb(&blink, lv_anim_path_ease_in_out);
    lv_anim_start(&blink);
}

static void blink_timer_cb(lv_timer_t *timer)
{
    static const uint32_t blink_intervals[] = { 2300, 3100, 1900, 420, 2700 };

    start_blink(s_left_eye);
    start_blink(s_right_eye);
    lv_timer_set_period(timer, blink_intervals[s_blink_count]);
    s_blink_count = (s_blink_count + 1) % 5;
}

static void set_pupil_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void move_pupil(lv_obj_t *pupil, int32_t target_x)
{
    lv_anim_t gaze;

    lv_anim_init(&gaze);
    lv_anim_set_var(&gaze, pupil);
    lv_anim_set_exec_cb(&gaze, set_pupil_x);
    lv_anim_set_values(&gaze, lv_obj_get_x(pupil), target_x);
    lv_anim_set_duration(&gaze, 320);
    lv_anim_set_path_cb(&gaze, lv_anim_path_ease_in_out);
    lv_anim_start(&gaze);
}

static void gaze_timer_cb(lv_timer_t *timer)
{
    static const int32_t gaze_positions[] = { 16, 10, 4, 10 };
    int32_t target_x = gaze_positions[s_gaze_step];

    move_pupil(s_left_pupil, target_x);
    move_pupil(s_right_pupil, target_x);
    s_gaze_step = (s_gaze_step + 1) % 4;
    lv_timer_set_period(timer, s_gaze_step == 0 ? 2200 : 1400);
}

void screens_set_weather_data_ptr(const void *data)
{
    (void)data;
}

void create_screen_main(void)
{
    s_main_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_main_screen, 256, 64);
    lv_obj_set_style_bg_color(s_main_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_main_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_main_screen, 0, 0);
    lv_obj_set_style_pad_all(s_main_screen, 0, 0);
    lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_left_eye = create_eye(s_main_screen, 76);
    s_right_eye = create_eye(s_main_screen, 140);
    s_left_pupil = create_pupil(s_left_eye);
    s_right_pupil = create_pupil(s_right_eye);

    lv_screen_load(s_main_screen);
    lv_timer_create(blink_timer_cb, 2000, NULL);
    lv_timer_create(gaze_timer_cb, 900, NULL);
}

void tick_screen_main(void)
{
    /* Eye motion is driven by LVGL animations. */
}

void create_screens(void)
{
    create_screen_main();
}
