#include "screens.h"
#include "vars.h"
#include "ssd1322_driver.h"
#include "weather_chart.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

objects_t objects;
lv_obj_t *tick_value_change_obj;
uint32_t active_theme_index = 0;

static bool show_weather = false;
static volatile bool pending_toggle = false;

void screens_request_toggle(void)
{
    pending_toggle = true;
}

void create_screen_main() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 256, 64);

    lv_obj_set_style_bg_color(obj, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    static lv_style_t style_time;
    lv_style_init(&style_time);
    lv_style_set_text_color(&style_time, lv_color_make(0x44, 0x44, 0x44));
    lv_style_set_text_font(&style_time, &lv_font_montserrat_48);

    lv_obj_t *label = lv_label_create(obj);
    objects.hour_label = label;
    lv_obj_set_pos(label, 48, 8);
    lv_obj_set_size(label, 64, 48);
    lv_label_set_text(label, "00");
    lv_obj_add_style(label, &style_time, 0);

    label = lv_label_create(obj);
    objects.colon_label = label;
    lv_obj_set_pos(label, 112, 8);
    lv_obj_set_size(label, 32, 48);
    lv_label_set_text(label, ":");
    lv_obj_add_style(label, &style_time, 0);

    label = lv_label_create(obj);
    objects.minute_label = label;
    lv_obj_set_pos(label, 132, 8);
    lv_obj_set_size(label, 64, 48);
    lv_label_set_text(label, "00");
    lv_obj_add_style(label, &style_time, 0);

    /* Create weather chart overlay (hidden by default) */
    weather_chart_create(obj);

    tick_screen_main();
}

void tick_screen_main() {
    /* Process pending toggle (safe: called from LVGL task context) */
    if (pending_toggle) {
        pending_toggle = false;
        screens_toggle_weather();
        return;
    }

    static int tick_count = -1;
    tick_count++;

    if (show_weather) return;

    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);

    char buf[8];
    strftime(buf, sizeof(buf), "%H", &timeinfo);
    lv_label_set_text(objects.hour_label, buf);

    strftime(buf, sizeof(buf), "%M", &timeinfo);
    lv_label_set_text(objects.minute_label, buf);

    /* Toggle colon visibility every second */
    if (tick_count % 2 == 0) {
        lv_obj_set_style_text_color(objects.colon_label, lv_color_make(0x44, 0x44, 0x44), 0);
    } else {
        lv_obj_set_style_text_color(objects.colon_label, lv_color_make(0x00, 0x00, 0x00), 0);
    }

    lv_area_t full_screen = {0, 0, LCD_H_RES - 1, LCD_V_RES - 1};
    lv_obj_invalidate_area(lv_screen_active(), &full_screen);
    lv_refr_now(NULL);
}

void screens_toggle_weather(void)
{
    show_weather = !show_weather;

    if (show_weather) {
        if (!weather_chart_is_visible()) {
            lv_obj_add_flag(objects.hour_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.colon_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.minute_label, LV_OBJ_FLAG_HIDDEN);
            weather_chart_show();
        }
    } else {
        weather_chart_hide();
        lv_obj_remove_flag(objects.hour_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.colon_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.minute_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_area_t full_screen = {0, 0, LCD_H_RES - 1, LCD_V_RES - 1};
    lv_obj_invalidate_area(lv_screen_active(), &full_screen);
    lv_refr_now(NULL);
}

bool screens_is_weather_visible(void)
{
    return show_weather;
}

void screens_set_weather_data_ptr(const void *data)
{
    weather_chart_set_data((const weather_data_t *)data);
}

typedef void (*tick_screen_func_t)(void);
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
};
void tick_screen(int screen_index) {
    tick_screen_funcs[screen_index]();
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen_funcs[screenId - 1]();
}

void create_screens() {
    create_screen_main();
    lv_screen_load(objects.main);
}
