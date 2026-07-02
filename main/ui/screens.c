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
    /* Skip the tick entirely if the clock screen isn't the active one (e.g.
     * we're on the QR provisioning page). The QR page is fully static and
     * doesn't need a per-second refresh, and clock_screen_set_night_mode /
     * clock_screen_update_time / clock_screen_set_data all early-return on
     * !visible anyway, so this just short-circuits a few function calls. */
    lv_obj_t *active = lv_screen_active();
    if (active == NULL) return;

    /* BUG FIX: the previous code did a full-screen invalidation + lv_refr_now()
     * here, which forced a synchronous flush of the entire 128×64 every
     * second. spi_device_polling_transmit() in lvgl_flush_cb blocks for
     * 5-10 ms per flush. With lvgl_task at priority 5 hogging the CPU on
     * CPU 0, the IDLE task (which feeds its own WDT subscription by virtue
     * of being scheduled) was repeatedly starved past the 5 s WDT window.
     * The full-screen invalidate + sync flush is also wasteful for the
     * static QR provisioning page (it'd redraw the same pixels every
     * second). clock_screen_update_time() already invalidates just the
     * time-label area; let the next lv_timer_handler() (10 ms) flush it. */
    clock_screen_set_night_mode(clock_screen_is_night_time());

    /* Apply pending weather data from non-LVGL context (e.g. button callback) */
    if (pending_weather_data) {
        const weather_data_t *data = pending_weather_data;
        pending_weather_data = NULL;
        clock_screen_set_data(data);
    }

    /* Keep time display updated. This invalidates only the time-label area
     * (and re-draws the canvas) — no need to invalidate the full screen. */
    clock_screen_update_time();
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
