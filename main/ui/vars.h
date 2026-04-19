#ifndef LVGL_UI_VARS_H
#define LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

extern lv_obj_t *tick_value_change_obj;
extern uint32_t active_theme_index;

#ifdef __cplusplus
}
#endif

#endif // LVGL_UI_VARS_H
