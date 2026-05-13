#include "screens.h"
#include "vars.h"
#include "ssd1322_driver.h"
#include "clock_screen.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

objects_t objects;
lv_obj_t *tick_value_change_obj;
uint32_t active_theme_index = 0;

static const weather_data_t *volatile pending_weather_data = NULL;

void screens_set_weather_data_ptr(const void *data)
{
    /* Defer: will be applied from LVGL task context in tick_screen_main */
    pending_weather_data = (const weather_data_t *)data;
}

void create_screen_main() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 256, 64);

    lv_obj_set_style_bg_color(obj, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    /* Create weather chart (home page, always visible) */
    clock_screen_create(obj);
    clock_screen_show();

    /* Load screen BEFORE tick so lv_refr_now renders the correct (black) screen */
    lv_screen_load(objects.main);

    tick_screen_main();
}

void tick_screen_main()
{
    clock_screen_set_night_mode(clock_screen_is_night_time());

    /* Apply pending weather data from non-LVGL context (e.g. button callback) */
    if (pending_weather_data) {
        const weather_data_t *data = pending_weather_data;
        pending_weather_data = NULL;
        clock_screen_set_data(data);
    }

    /* Keep time display updated */
    clock_screen_update_time();

    lv_area_t full_screen = {0, 0, LCD_H_RES - 1, LCD_V_RES - 1};
    lv_obj_invalidate_area(lv_screen_active(), &full_screen);
    lv_refr_now(lv_disp_get_default());
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
}
