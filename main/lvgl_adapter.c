#include "lvgl_adapter.h"
#include "ssd1322_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "ui/ui.h"

static const char *TAG = "LVGL_ADAPTER";
static lv_display_t *g_disp = NULL;
static uint8_t *g_i4_buffer = NULL;

static void lvgl_task(void *arg);

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    static uint32_t flush_n;
    flush_n++;
    if (flush_n % 10 == 1) {
        ESP_LOGI(TAG, "FLUSH n=%lu", (unsigned long)flush_n);
    }

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

    xTaskCreate(lvgl_task, "lvgl_task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "LVGL adapter initialized");
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    esp_task_wdt_add(NULL);
    uint32_t count = 0;
    while (1) {
        esp_task_wdt_reset();
        lv_timer_handler();
        /* Call ui_tick every 100 iterations (~1 second) */
        count++;
        if (count % 100 == 0) {
            ui_tick();
            ESP_LOGI(TAG, "alive=%lu", (unsigned long)count);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

lv_display_t* lvgl_adapter_get_display(void)
{
    return g_disp;
}
