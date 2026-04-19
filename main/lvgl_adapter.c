#include "lvgl_adapter.h"
#include "ssd1322_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui.h"

static const char *TAG = "LVGL_ADAPTER";
static lv_display_t *g_disp = NULL;
static uint8_t *g_i4_buffer = NULL;
static bool ui_initialized = false;

static void lvgl_task(void *arg);

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int x_start = area->x1;
    int y_start = area->y1;
    int x_end = area->x2;
    int y_end = area->y2;

    int width = x_end - x_start + 1;
    int height = y_end - y_start + 1;
    size_t i4_len = (width / 2) * height;

    if (g_i4_buffer) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += 2) {
                int src_idx = y * width + x;
                int dst_idx = y * (width / 2) + (x / 2);

                uint8_t p0 = px_map[src_idx] >> 4;
                uint8_t p1 = px_map[src_idx + 1] >> 4;

                g_i4_buffer[dst_idx] = (p0 << 4) | p1;
            }
        }

        ssd1322_send_cmd(0x15);
        ssd1322_send_data((x_start / 4) + 0x1C);
        ssd1322_send_data((x_end / 4) + 0x1C);

        ssd1322_send_cmd(0x75);
        ssd1322_send_data(y_start);
        ssd1322_send_data(y_end);

        ssd1322_send_cmd(0x5C);

        gpio_set_level(PIN_NUM_DC, 1);

        spi_transaction_t t = {
            .length = i4_len * 8,
            .tx_buffer = g_i4_buffer
        };
        spi_device_polling_transmit(ssd1322_get_spi_handle(), &t);
    }

    lv_display_flush_ready(disp);
}

esp_err_t lvgl_adapter_init(void)
{
    lv_init();

    g_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!g_disp) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_L8);

    g_i4_buffer = heap_caps_malloc(LCD_H_RES * LCD_V_RES / 2, MALLOC_CAP_DMA);
    if (!g_i4_buffer) {
        ESP_LOGE(TAG, "Failed to allocate I4 buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t buf_size = LCD_H_RES * LCD_V_RES;
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(g_disp, buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_display_set_flush_cb(g_disp, lvgl_flush_cb);

    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "LVGL adapter initialized");
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    while (1) {
        lv_timer_handler();
        if (ui_initialized) {
            ui_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lvgl_adapter_set_ui_ready(void)
{
    ui_initialized = true;
}

lv_display_t* lvgl_adapter_get_display(void)
{
    return g_disp;
}
