/*******************************************************************************
 * Size: 10 px
 * Bpp: 2
 * Opts: --size 10 --bpp 2 --format lvgl --no-compress --font assets/fonts/ark-pixel-10px-monospaced-zh_cn.ttf --symbols  0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-_广播中 --output main/font_station.c --lv-font-name lv_font_station
 ******************************************************************************/

#include "lvgl.h"

#ifndef LV_FONT_STATION
#define LV_FONT_STATION 1
#endif

#if LV_FONT_STATION

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+002D "-" */
    0xff,

    /* U+002E "." */
    0xc0,

    /* U+0030 "0" */
    0x3c, 0xc3, 0xcf, 0xf3, 0xc3, 0xc3, 0x3c,

    /* U+0031 "1" */
    0x33, 0xc3, 0xc, 0x30, 0xcf, 0xc0,

    /* U+0032 "2" */
    0x3c, 0xc3, 0x3, 0xc, 0x30, 0xc0, 0xff,

    /* U+0033 "3" */
    0x3c, 0xc3, 0x3, 0xc, 0x3, 0xc3, 0x3c,

    /* U+0034 "4" */
    0xf, 0x33, 0x33, 0xc3, 0xc3, 0xff, 0x3,

    /* U+0035 "5" */
    0xff, 0xc0, 0xc0, 0xfc, 0x3, 0x3, 0xfc,

    /* U+0036 "6" */
    0x3c, 0xc3, 0xc0, 0xfc, 0xc3, 0xc3, 0x3c,

    /* U+0037 "7" */
    0xff, 0x3, 0x3, 0xc, 0xc, 0x30, 0x30,

    /* U+0038 "8" */
    0x3c, 0xc3, 0xc3, 0x3c, 0xc3, 0xc3, 0x3c,

    /* U+0039 "9" */
    0x3c, 0xc3, 0xc3, 0x3f, 0x3, 0xc3, 0x3c,

    /* U+0041 "A" */
    0x3c, 0xc3, 0xc3, 0xc3, 0xff, 0xc3, 0xc3,

    /* U+0042 "B" */
    0xfc, 0xc3, 0xc3, 0xfc, 0xc3, 0xc3, 0xfc,

    /* U+0043 "C" */
    0x3c, 0xc3, 0xc0, 0xc0, 0xc0, 0xc3, 0x3c,

    /* U+0044 "D" */
    0xfc, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xfc,

    /* U+0045 "E" */
    0xff, 0xc0, 0xc0, 0xfc, 0xc0, 0xc0, 0xff,

    /* U+0046 "F" */
    0xff, 0xc0, 0xc0, 0xfc, 0xc0, 0xc0, 0xc0,

    /* U+0047 "G" */
    0x3c, 0xc3, 0xc0, 0xc0, 0xcf, 0xc3, 0x3f,

    /* U+0048 "H" */
    0xc3, 0xc3, 0xc3, 0xff, 0xc3, 0xc3, 0xc3,

    /* U+0049 "I" */
    0xff, 0xc, 0xc, 0xc, 0xc, 0xc, 0xff,

    /* U+004A "J" */
    0x3, 0x3, 0x3, 0x3, 0xc3, 0xc3, 0x3c,

    /* U+004B "K" */
    0xc3, 0xc3, 0xcc, 0xf0, 0xcc, 0xc3, 0xc3,

    /* U+004C "L" */
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xff,

    /* U+004D "M" */
    0xc3, 0xff, 0xff, 0xc3, 0xc3, 0xc3, 0xc3,

    /* U+004E "N" */
    0xc3, 0xf3, 0xf3, 0xcf, 0xcf, 0xc3, 0xc3,

    /* U+004F "O" */
    0x3c, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x3c,

    /* U+0050 "P" */
    0xfc, 0xc3, 0xc3, 0xfc, 0xc0, 0xc0, 0xc0,

    /* U+0051 "Q" */
    0x3c, 0xc3, 0xc3, 0xc3, 0xc3, 0xcc, 0x33,

    /* U+0052 "R" */
    0xfc, 0xc3, 0xc3, 0xfc, 0xcc, 0xc3, 0xc3,

    /* U+0053 "S" */
    0x3c, 0xc3, 0xc0, 0x3c, 0x3, 0xc3, 0x3c,

    /* U+0054 "T" */
    0xff, 0xc, 0xc, 0xc, 0xc, 0xc, 0xc,

    /* U+0055 "U" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x3c,

    /* U+0056 "V" */
    0xc3, 0xc3, 0xc3, 0xcc, 0xcc, 0xcc, 0xf0,

    /* U+0057 "W" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xff, 0xff, 0xc3,

    /* U+0058 "X" */
    0xc3, 0xc3, 0xc3, 0x3c, 0xc3, 0xc3, 0xc3,

    /* U+0059 "Y" */
    0xc3, 0xc3, 0x33, 0x33, 0xc, 0xc, 0xc,

    /* U+005A "Z" */
    0xff, 0x3, 0xc, 0x30, 0x30, 0xc0, 0xff,

    /* U+005F "_" */
    0xff,

    /* U+0061 "a" */
    0x3c, 0x3, 0x3f, 0xc3, 0x3f,

    /* U+0062 "b" */
    0xc0, 0xc0, 0xfc, 0xc3, 0xc3, 0xc3, 0xfc,

    /* U+0063 "c" */
    0x3c, 0xc3, 0xc0, 0xc3, 0x3c,

    /* U+0064 "d" */
    0x3, 0x3, 0x3f, 0xc3, 0xc3, 0xc3, 0x3f,

    /* U+0065 "e" */
    0x3c, 0xc3, 0xff, 0xc0, 0x3c,

    /* U+0066 "f" */
    0xf, 0x30, 0xff, 0x30, 0x30, 0x30, 0x30,

    /* U+0067 "g" */
    0x3f, 0xc3, 0xc3, 0x3f, 0x3, 0x3c,

    /* U+0068 "h" */
    0xc0, 0xc0, 0xfc, 0xc3, 0xc3, 0xc3, 0xc3,

    /* U+0069 "i" */
    0xc, 0x0, 0xfc, 0xc, 0xc, 0xc, 0xff,

    /* U+006A "j" */
    0xc, 0xf, 0xc3, 0xc, 0x30, 0xfc,

    /* U+006B "k" */
    0xc0, 0xc0, 0xc3, 0xcc, 0xf0, 0xcc, 0xc3,

    /* U+006C "l" */
    0xf0, 0x30, 0x30, 0x30, 0x30, 0x30, 0xf,

    /* U+006D "m" */
    0xc3, 0xff, 0xff, 0xc3, 0xc3,

    /* U+006E "n" */
    0xfc, 0xc3, 0xc3, 0xc3, 0xc3,

    /* U+006F "o" */
    0x3c, 0xc3, 0xc3, 0xc3, 0x3c,

    /* U+0070 "p" */
    0xfc, 0xc3, 0xc3, 0xfc, 0xc0, 0xc0,

    /* U+0071 "q" */
    0x3f, 0xc3, 0xc3, 0x3f, 0x3, 0x3,

    /* U+0072 "r" */
    0xcf, 0xf0, 0xc0, 0xc0, 0xc0,

    /* U+0073 "s" */
    0x3f, 0xc0, 0x3c, 0x3, 0xfc,

    /* U+0074 "t" */
    0x30, 0x30, 0xff, 0x30, 0x30, 0x30, 0xf,

    /* U+0075 "u" */
    0xc3, 0xc3, 0xc3, 0xc3, 0x3f,

    /* U+0076 "v" */
    0xc3, 0xc3, 0xcc, 0xcc, 0xf0,

    /* U+0077 "w" */
    0xc3, 0xc3, 0xff, 0xff, 0xc3,

    /* U+0078 "x" */
    0xc3, 0xc3, 0x3c, 0xc3, 0xc3,

    /* U+0079 "y" */
    0xc3, 0xc3, 0x33, 0x33, 0xc, 0xf0,

    /* U+007A "z" */
    0xff, 0xc, 0x30, 0xc0, 0xff,

    /* U+4E2D "中" */
    0x0, 0xc0, 0x0, 0x30, 0xf, 0xff, 0xff, 0x3,
    0x3, 0xc0, 0xc0, 0xff, 0xff, 0xf0, 0xc, 0x0,
    0x3, 0x0, 0x0, 0xc0, 0x0,

    /* U+5E7F "广" */
    0x0, 0x30, 0xf, 0xff, 0xf3, 0x0, 0x0, 0xc0,
    0x0, 0x30, 0x0, 0xc, 0x0, 0x3, 0x0, 0x0,
    0xc0, 0x0, 0xc0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 80, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 80, .box_w = 4, .box_h = 1, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 1, .adv_w = 80, .box_w = 1, .box_h = 1, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 2, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 9, .adv_w = 80, .box_w = 3, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 15, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 22, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 29, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 36, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 43, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 50, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 57, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 64, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 71, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 78, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 85, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 92, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 99, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 106, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 113, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 127, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 134, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 141, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 148, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 155, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 162, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 169, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 176, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 183, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 197, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 204, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 211, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 218, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 225, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 232, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 239, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 246, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 253, .adv_w = 80, .box_w = 4, .box_h = 1, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 254, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 259, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 266, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 271, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 278, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 283, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 290, .adv_w = 80, .box_w = 4, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 296, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 303, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 310, .adv_w = 80, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 316, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 323, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 330, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 335, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 340, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 345, .adv_w = 80, .box_w = 4, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 351, .adv_w = 80, .box_w = 4, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 357, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 362, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 367, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 374, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 379, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 384, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 389, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 394, .adv_w = 80, .box_w = 4, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 400, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 405, .adv_w = 160, .box_w = 9, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 426, .adv_w = 160, .box_w = 9, .box_h = 9, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0xd, 0xe
};

static const uint16_t unicode_list_5[] = {
    0x0, 0x1052
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 15, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    },
    {
        .range_start = 48, .range_length = 10, .glyph_id_start = 4,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 65, .range_length = 26, .glyph_id_start = 14,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 95, .range_length = 1, .glyph_id_start = 40,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 97, .range_length = 26, .glyph_id_start = 41,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 20013, .range_length = 4179, .glyph_id_start = 67,
        .unicode_list = unicode_list_5, .glyph_id_ofs_list = NULL, .list_length = 2, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 6,
    .bpp = 2,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_station = {
#else
lv_font_t lv_font_station = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 9,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_STATION*/

