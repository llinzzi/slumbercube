#include "screens.h"
#include "clock_screen.h"

static lv_obj_t *s_main_screen = NULL;

static const weather_data_t *volatile pending_weather_data = NULL;

void screens_set_weather_data_ptr(const void *data)
{
    /* Defer: will be applied from LVGL task context in tick_screen_main */
    pending_weather_data = (const weather_data_t *)data;
}

void create_screen_main(void) {
    lv_obj_t *obj = lv_obj_create(0);
    s_main_screen = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 256, 64);

    lv_obj_set_style_bg_color(obj, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    /* Create weather chart (home page, always visible) */
    clock_screen_create(obj);
    clock_screen_show();

    /* Load screen BEFORE tick so lv_refr_now renders the correct (black) screen */
    lv_screen_load(s_main_screen);

    tick_screen_main();
}

void tick_screen_main(void)
{
    /* LVGL's active-screen check is the single source of truth for whether
     * the clock page is on top. config_screen.c swaps in a QR root for
     * provisioning; while it's loaded we no-op so we don't waste cycles
     * updating an off-screen canvas. */
    if (lv_screen_active() != s_main_screen) return;

    /* clock_screen_update_time() invalidates only the time-label area;
     * no need to invalidate the full screen here. */
    clock_screen_set_night_mode(clock_screen_is_night_time());

    /* Apply pending weather data from non-LVGL context (e.g. button callback) */
    if (pending_weather_data) {
        const weather_data_t *data = pending_weather_data;
        pending_weather_data = NULL;
        clock_screen_set_data(data);
    }

    clock_screen_update_time();
}

void create_screens(void) {
    create_screen_main();
}