#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* 8-bit grayscale (L8) for SSD1322 */
#define LV_COLOR_DEPTH 8

/* Tick source via ESP timer */
#define LV_USE_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (esp_timer_get_time() / 1000LL)

/* Use custom memory allocation (PSRAM if available, else DRAM) */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC   heap_caps_malloc
#define LV_MEM_CUSTOM_FREE    heap_caps_free
#define LV_MEM_CUSTOM_REALLOC heap_caps_realloc

/* Large font support (full CJK requires >1MB bitmap) */
#define LV_FONT_FMT_TXT_LARGE 1

/* Fonts */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_DEFAULT &lv_font_station

/* Widgets used */
#define LV_USE_LABEL  1
#define LV_USE_CANVAS 1
#define LV_USE_BAR    1
#define LV_USE_LINE   1

/* Disable unused widgets to save flash */
#define LV_USE_BTN          0
#define LV_USE_IMG          0
#define LV_USE_ARC          0
#define LV_USE_CHART        0
#define LV_USE_DROPDOWN     0
#define LV_USE_SLIDER       0
#define LV_USE_SWITCH       0
#define LV_USE_TEXTAREA     0
#define LV_USE_KEYBOARD     0
#define LV_USE_ROLLER       0
#define LV_USE_TABLE        0
#define LV_USE_LIST         0
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      0
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0
#define LV_USE_CHECKBOX     0
#define LV_USE_LED          0
#define LV_USE_IMGBTN       0
#define LV_USE_BTNMATRIX    0
#define LV_USE_CALENDAR     0
#define LV_USE_CONTAINER     1

/* Disable animation, GPU, file system, etc. */
#define LV_USE_ANIMATION    0
#define LV_USE_DRAW_SW      1
#define LV_USE_DRAW_VGLITE  0
#define LV_USE_DRAW_PXP     0
#define LV_USE_DRAW_DAVE2D  0
#define LV_USE_FS_STDIO     0
#define LV_USE_FS_FATFS     0
#define LV_USE_PNG          0
#define LV_USE_BMP          0
#define LV_USE_SJPG         0
#define LV_USE_GIF          0
#define LV_USE_QRCODE       1
#define LV_USE_BARCODE      0
#define LV_USE_LZ4          0
#define LV_USE_FFMPEG       0

/* Log level */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO

#endif /* LV_CONF_H */
