#include "weather_chart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "WEATHER_CHART";

#define CANVAS_W 256
#define CANVAS_H 64
#define CHART_HOURS 12
#define MAX_RAINDROPS 20

#define PT_X(i) (14 + (i) * 20)

#define COL_CURVE    0x99
#define COL_POINT    0xCC
#define COL_TEMP     0xBB
#define COL_AXIS     0x66
#define COL_HOUR     0x88
#define COL_FIRST    0xBB
#define COL_RAIN     0x99

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static uint8_t *canvas_buf = NULL;
static lv_obj_t *temp_labels[CHART_HOURS];
static lv_obj_t *hour_labels[CHART_HOURS];
static lv_obj_t *first_time_label = NULL;
static lv_timer_t *anim_timer = NULL;

static const weather_data_t *weather = NULL;
static bool visible = false;

typedef struct {
    float x, y;
    float speed;
    bool active;
} raindrop_t;
static raindrop_t raindrops[MAX_RAINDROPS];
static int drop_count = 0;

static int temp_to_y(int temp, int t_min, int t_max)
{
    if (t_max <= t_min) return 26;
    float frac = (float)(temp - t_min) / (float)(t_max - t_min);
    return 45 - (int)(frac * 37.0f);
}

static void draw_chart(void)
{
    if (!canvas || !canvas_buf || !weather || !weather->valid) return;

    int count = weather->count;
    if (count > CHART_HOURS) count = CHART_HOURS;
    if (count < 2) return;

    int t_min = 99, t_max = -99;
    for (int i = 0; i < count; i++) {
        int t = weather->hourly[i].temp;
        if (t < t_min) t_min = t;
        if (t > t_max) t_max = t;
    }
    if (t_max <= t_min) { t_min = t_max - 1; t_max = t_min + 2; }

    lv_canvas_fill_bg(canvas, lv_color_make(0, 0, 0), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_color_t curve_col = lv_color_make(COL_CURVE, COL_CURVE, COL_CURVE);
    lv_color_t point_col = lv_color_make(COL_POINT, COL_POINT, COL_POINT);
    lv_color_t axis_col  = lv_color_make(COL_AXIS, COL_AXIS, COL_AXIS);
    lv_color_t rain_col  = lv_color_make(COL_RAIN, COL_RAIN, COL_RAIN);

    /* Temperature curve */
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = curve_col;
    line_dsc.width = 1;

    for (int i = 0; i < count - 1; i++) {
        int x1 = PT_X(i);
        int y1 = temp_to_y(weather->hourly[i].temp, t_min, t_max);
        int x2 = PT_X(i + 1);
        int y2 = temp_to_y(weather->hourly[i + 1].temp, t_min, t_max);
        line_dsc.p1.x = x1;
        line_dsc.p1.y = y1;
        line_dsc.p2.x = x2;
        line_dsc.p2.y = y2;
        lv_draw_line(&layer, &line_dsc);
    }

    /* Data points */
    lv_draw_fill_dsc_t pt_dsc;
    lv_draw_fill_dsc_init(&pt_dsc);
    pt_dsc.color = point_col;
    pt_dsc.radius = 2;

    for (int i = 0; i < count; i++) {
        int x = PT_X(i);
        int y = temp_to_y(weather->hourly[i].temp, t_min, t_max);
        lv_area_t a = {x - 2, y - 2, x + 2, y + 2};
        lv_draw_fill(&layer, &pt_dsc, &a);
    }

    /* Time axis */
    lv_draw_line_dsc_t axis_line;
    lv_draw_line_dsc_init(&axis_line);
    axis_line.color = axis_col;
    axis_line.width = 1;
    axis_line.p1.x = 8;
    axis_line.p1.y = 48;
    axis_line.p2.x = 248;
    axis_line.p2.y = 48;
    lv_draw_line(&layer, &axis_line);

    /* Raindrops */
    if (drop_count > 0) {
        lv_draw_fill_dsc_t rain_dsc;
        lv_draw_fill_dsc_init(&rain_dsc);
        rain_dsc.color = rain_col;
        rain_dsc.radius = 0;
        for (int i = 0; i < drop_count; i++) {
            if (!raindrops[i].active) continue;
            int rx = (int)raindrops[i].x;
            int ry = (int)raindrops[i].y;
            if (ry < 0 || ry > 46) continue;
            lv_area_t ra = {rx, ry, rx + 1, ry + 3};
            lv_draw_fill(&layer, &rain_dsc, &ra);
        }
    }

    lv_canvas_finish_layer(canvas, &layer);

    /* Update temperature labels */
    for (int i = 0; i < count; i++) {
        int x = PT_X(i);
        int y = temp_to_y(weather->hourly[i].temp, t_min, t_max);
        int label_y = y - 16;
        if (label_y < 2) label_y = y + 8;

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", weather->hourly[i].temp);
        lv_label_set_text(temp_labels[i], buf);
        lv_obj_set_pos(temp_labels[i], x - 10, label_y);
        lv_obj_set_size(temp_labels[i], 20, 14);
        lv_obj_set_style_text_align(temp_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Update hour labels */
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    for (int i = 0; i < count; i++) {
        int x = PT_X(i);
        if (i == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
            lv_label_set_text(first_time_label, buf);
            lv_obj_set_pos(first_time_label, x - 14, 50);
            lv_obj_set_size(first_time_label, 28, 10);
            lv_obj_set_style_text_align(first_time_label, LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_text(hour_labels[i], "");
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", weather->hourly[i].hour);
            lv_label_set_text(hour_labels[i], buf);
            lv_obj_set_pos(hour_labels[i], x - 6, 50);
            lv_obj_set_size(hour_labels[i], 16, 10);
            lv_obj_set_style_text_align(hour_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* Hide unused labels */
    for (int i = count; i < CHART_HOURS; i++) {
        lv_label_set_text(temp_labels[i], "");
        lv_label_set_text(hour_labels[i], "");
    }
}

static void anim_cb(lv_timer_t *t)
{
    (void)t;
    if (!visible || !weather || !weather->valid || drop_count == 0) return;

    for (int i = 0; i < drop_count; i++) {
        if (!raindrops[i].active) continue;
        raindrops[i].y += raindrops[i].speed * 0.1f;
        if (raindrops[i].y > 47) {
            int idx = i % (weather->count > 0 ? weather->count : 1);
            int cx = PT_X(idx % CHART_HOURS);
            raindrops[i].x = cx + (float)((i * 7 + 3) % 15 - 7);
            raindrops[i].y = -(float)((i * 5) % 24);
            if (idx < weather->count) {
                raindrops[i].speed = 10.0f + weather->hourly[idx].rain_prob / 5.0f;
            }
        }
    }
    draw_chart();
}

lv_obj_t *weather_chart_create(lv_obj_t *parent)
{
    container = lv_obj_create(parent);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, CANVAS_W, CANVAS_H);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);

    canvas_buf = heap_caps_malloc(CANVAS_W * CANVAS_H, MALLOC_CAP_8BIT);
    if (!canvas_buf) {
        ESP_LOGE(TAG, "OOM for canvas buffer (%d)", CANVAS_W * CANVAS_H);
        return container;
    }
    memset(canvas_buf, 0, CANVAS_W * CANVAS_H);

    canvas = lv_canvas_create(container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_L8);
    lv_obj_set_pos(canvas, 0, 0);

    for (int i = 0; i < CHART_HOURS; i++) {
        temp_labels[i] = lv_label_create(container);
        lv_obj_set_style_text_color(temp_labels[i], lv_color_make(COL_TEMP, COL_TEMP, COL_TEMP), 0);
        lv_obj_set_style_text_font(temp_labels[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(temp_labels[i], "");
    }

    first_time_label = lv_label_create(container);
    lv_obj_set_style_text_color(first_time_label, lv_color_make(COL_FIRST, COL_FIRST, COL_FIRST), 0);
    lv_obj_set_style_text_font(first_time_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(first_time_label, "");

    for (int i = 0; i < CHART_HOURS; i++) {
        hour_labels[i] = lv_label_create(container);
        lv_obj_set_style_text_color(hour_labels[i], lv_color_make(COL_HOUR, COL_HOUR, COL_HOUR), 0);
        lv_obj_set_style_text_font(hour_labels[i], &lv_font_montserrat_8, 0);
        lv_label_set_text(hour_labels[i], "");
    }

    anim_timer = lv_timer_create(anim_cb, 100, NULL);

    return container;
}

void weather_chart_set_data(const weather_data_t *data)
{
    weather = data;
    if (!weather || !weather->valid) return;

    memset(raindrops, 0, sizeof(raindrops));
    drop_count = 0;

    int max_prob = 0;
    int hc = weather->count < CHART_HOURS ? weather->count : CHART_HOURS;
    for (int i = 0; i < hc; i++) {
        if (weather->hourly[i].rain_prob > max_prob)
            max_prob = weather->hourly[i].rain_prob;
    }

    if (max_prob >= 30) {
        drop_count = max_prob / 10;
        if (drop_count > MAX_RAINDROPS) drop_count = MAX_RAINDROPS;
        float base_speed = 10.0f + max_prob / 5.0f;
        for (int i = 0; i < drop_count; i++) {
            int idx = i % (hc > 0 ? hc : 1);
            int cx = PT_X(idx % CHART_HOURS);
            raindrops[i].x = cx + (float)((i * 7) % 15 - 7);
            raindrops[i].y = -(float)((i * 13) % 60);
            raindrops[i].speed = base_speed + (i % 3) * 2.0f;
            raindrops[i].active = true;
        }
    }

    draw_chart();
}

void weather_chart_show(void)
{
    if (!container) return;
    visible = true;
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
}

void weather_chart_hide(void)
{
    if (!container) return;
    visible = false;
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

bool weather_chart_is_visible(void)
{
    return visible;
}
