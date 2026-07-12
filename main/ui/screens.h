#ifndef LVGL_UI_SCREENS_H
#define LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void create_screens(void);
void tick_screen_main(void);

/* Queue a weather pointer from any task context; tick_screen_main() applies
 * it on the next LVGL-task tick. */
void screens_set_weather_data_ptr(const void *data);

#ifdef __cplusplus
}
#endif

#endif // LVGL_UI_SCREENS_H