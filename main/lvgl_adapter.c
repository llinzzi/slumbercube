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

        /* Protect the entire SPI command+data sequence from task
         * preemption. Higher-priority tasks (WiFi at 23, audio)
         * preempting spi_device_polling_transmit mid-transfer
         * starves the SSD1322 of SPI clock, corrupting its state
         * machine and causing progressive row shifting ("花屏"). */
        portDISABLE_INTERRUPTS();

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

        portENABLE_INTERRUPTS();
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

    /* 1/4 screen partial buffer to reduce DMA memory usage for TLS */
    size_t buf_size = LCD_H_RES * LCD_V_RES / 4;
    size_t i4_buf_size = buf_size / 2;

    g_i4_buffer = heap_caps_malloc(i4_buf_size, MALLOC_CAP_DMA);
    if (!g_i4_buffer) {
        ESP_LOGE(TAG, "Failed to allocate I4 buffer (%d)", (int)i4_buf_size);
        return ESP_ERR_NO_MEM;
    }

    void *buf1 = heap_caps_calloc(1, buf_size, MALLOC_CAP_DMA);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(g_disp, buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_display_set_flush_cb(g_disp, lvgl_flush_cb);

    /* Anti-white-flash: paint default screen black before any render. */
    lv_obj_t *boot_scr = lv_display_get_screen_active(g_disp);
    lv_obj_set_style_bg_color(boot_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, LV_PART_MAIN);

    xTaskCreate(lvgl_task, "lvgl_task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "LVGL adapter initialized");
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t count = 0;
    while (1) {
        lv_timer_handler();
        count++;
        if (count % 100 == 0) {
            ui_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Force a synchronous full-screen render from the main task.
 * Called from the main loop every second. We can't use lv_timer_handler
 * incremental rendering — SSD1322 column addressing corrupts partial
 * flushes. lv_refr_now sends the complete framebuffer which is always
 * 4-pixel-aligned and matches the boot-time render path. */
void lvgl_adapter_refr_now(void)
{
    if (g_disp) lv_refr_now(g_disp);
}

lv_display_t* lvgl_adapter_get_display(void)
{
    return g_disp;
}
