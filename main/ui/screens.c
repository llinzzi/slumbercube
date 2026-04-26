#include "screens.h"
#include "vars.h"
#include "ssd1322_driver.h"
#include <string.h>
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
    static int tick_count = -1;
    tick_count++;

    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);

    char buf[32];
    if (tick_count % 2 == 0) {
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    } else {
        strftime(buf, sizeof(buf), "%H %M", &timeinfo);
    }
    lv_label_set_text(objects.time_label, buf);

    lv_area_t full_screen = {0, 0, LCD_H_RES - 1, LCD_V_RES - 1};
    lv_obj_invalidate_area(lv_screen_active(), &full_screen);
    lv_refr_now(NULL);
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
