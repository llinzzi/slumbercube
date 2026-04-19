#ifndef LVGL_UI_GUI_H
#define LVGL_UI_GUI_H

#include <lvgl.h>

#include "screens.h"
#include "actions.h"
#include "vars.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
void ui_tick(void);
void loadScreen(enum ScreensEnum screenId);

#ifdef __cplusplus
}
#endif

#endif // LVGL_UI_GUI_H
