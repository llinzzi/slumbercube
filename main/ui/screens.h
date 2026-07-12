#ifndef LVGL_UI_SCREENS_H
#define LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
};

void create_screen_main(void);
void tick_screen_main(void);
void tick_screen(int screen_index);
void create_screens(void);

void screens_set_weather_data_ptr(const void *data);

#ifdef __cplusplus
}
#endif

#endif // LVGL_UI_SCREENS_H
