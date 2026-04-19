#include "screens.h"
#include "vars.h"
#include <time.h>
#include <sys/time.h>

objects_t objects;
lv_obj_t *tick_value_change_obj;
uint32_t active_theme_index = 0;

void create_screen_main() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 256, 64);

    lv_obj_set_style_bg_color(obj, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    {
        lv_obj_t *parent_obj = obj;
        lv_obj_t *label = lv_label_create(parent_obj);
        objects.time_label = label;
        lv_obj_set_pos(label, 80, 20);
        lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_label_set_text(label, "00:00");
        lv_obj_set_style_text_color(label, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    }

    tick_screen_main();
}

void tick_screen_main() {
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);

    static char time_str[16] = {0};
    strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
    lv_label_set_text(objects.time_label, time_str);
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
