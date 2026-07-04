#include "lvgl_adapter.h"
#include "ssd1322_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ui/ui.h"

static const char *TAG = "LVGL_ADAPTER";
static lv_display_t *g_disp = NULL;
static uint8_t *g_i4_buffer = NULL;
static SemaphoreHandle_t g_lvgl_mutex = NULL;

static void lvgl_task(void *arg);

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int x_start = area->x1;
    int y_start = area->y1;
    int x_end = area->x2;
    int y_end = area->y2;

    int width = x_end - x_start + 1;
    int height = y_end - y_start + 1;

    if (g_i4_buffer) {
        /* SSD1322 列地址粒度是 4 像素（1 列 = 2 字节 GDDRAM = 4 个 4-bit 像素），
         * 无法表达非 4 对齐的 X。因此把 g_i4_buffer 维护为全屏 I4 镜像：
         * 先把 area 的 L8 像素按 nibble 写入镜像的正确位置，再以整宽
         * (列 0x1C..0x5B = x0..255) × area 行范围一次性发送。每次 SPI 传输
         * 都整宽 4 对齐，任意 partial area 均正确，避免列窗口与数据长度
         * 不匹配导致的花屏。 */
        const int row_bytes = LCD_H_RES / 2;  /* 128 字节/行 I4 */

        for (int y = 0; y < height; y++) {
            int dst_row = (y_start + y) * row_bytes;
            int src_row = y * width;
            for (int x = 0; x < width; x++) {
                int abs_x = x_start + x;
                int byte_idx = dst_row + (abs_x / 2);
                uint8_t nib = px_map[src_row + x] >> 4;
                if (abs_x & 1) {
                    g_i4_buffer[byte_idx] = (g_i4_buffer[byte_idx] & 0xF0) | (nib & 0x0F);
                } else {
                    g_i4_buffer[byte_idx] = (g_i4_buffer[byte_idx] & 0x0F) | (nib << 4);
                }
            }
        }

        /* 整宽窗口：列 0x1C..0x5B（256 像素），行 y_start..y_end */
        ssd1322_send_cmd(0x15);
        ssd1322_send_data(0x1C);
        ssd1322_send_data(0x5B);

        ssd1322_send_cmd(0x75);
        ssd1322_send_data(y_start);
        ssd1322_send_data(y_end);

        ssd1322_send_cmd(0x5C);

        gpio_set_level(PIN_NUM_DC, 1);

        spi_transaction_t t = {
            .length = (size_t)row_bytes * height * 8,
            .tx_buffer = g_i4_buffer + (size_t)y_start * row_bytes
        };
        /* Queue + timed wait: the old polling_transmit had no timeout and
         * could hang forever if the SPI DMA completion interrupt was lost
         * (CS is hardwired to GND so the bus is always selected). A hung
         * SPI transfer inside lv_timer_handler starves IDLE and triggers
         * the task watchdog after 5 s. */
        spi_device_queue_trans(ssd1322_get_spi_handle(), &t, portMAX_DELAY);
        spi_transaction_t *ret_trans = NULL;
        esp_err_t spi_err = spi_device_get_trans_result(
            ssd1322_get_spi_handle(), &ret_trans, pdMS_TO_TICKS(100));
        if (spi_err != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit timeout, display may be corrupted");
        }
    }

    lv_display_flush_ready(disp);
}

esp_err_t lvgl_adapter_init(void)
{
    lv_init();

    g_lvgl_mutex = xSemaphoreCreateMutex();
    if (!g_lvgl_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    g_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!g_disp) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_L8);

    /* 1/4 screen partial buffer to reduce DMA memory usage for TLS */
    size_t buf_size = LCD_H_RES * LCD_V_RES / 4;

    /* 全屏 I4 镜像：128 字节/行 × 64 行 = 8192 字节。作为持久 GDDRAM 镜像，
     * 让 flush 回调能以整宽发送任意 partial area（见 lvgl_flush_cb 注释）。 */
    size_t i4_buf_size = (LCD_H_RES / 2) * LCD_V_RES;

    g_i4_buffer = heap_caps_calloc(1, i4_buf_size, MALLOC_CAP_DMA);
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

    /* Critical: paint the default LVGL screen BLACK right now. The lvgl_task
     * is about to start calling lv_timer_handler() at 10 ms intervals, which
     * triggers a flush of whatever the active screen is — and at this moment
     * nobody has loaded a real screen yet, so it'd be LVGL's default white
     * background. The OLED is still off (ssd1322_display_on hasn't run), so
     * the white never reaches the panel — BUT ssd1322_flush_cb writes the
     * same colour into GDDRAM. When ssd1322_display_on() flips the panel
     * on a frame later, the user sees the stale GDDRAM contents flash white
     * for one frame before the real screen's first flush lands.
     *
     * Forcing the default screen to black here means those early flushes
     * write black into GDDRAM, which is invisible against the still-off panel.
     * Once the real screen (QR or clock) is loaded and force-flushed, it
     * overwrites the black and the user sees the intended first frame. */
    lv_obj_t *boot_scr = lv_display_get_screen_active(g_disp);
    lv_obj_set_style_bg_color(boot_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Priority 3 — below audio mixer (5) and HTTP download (6) so that
     * the blocking SPI flush (~10ms) in lv_timer_handler can't starve the
     * audio decoder and cause I2S underrun stutter. Priority 3 is high enough
     * to get scheduled regularly (above idle at 0). */
    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "LVGL adapter initialized");
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t count = 0;
    while (1) {
        uint32_t t0 = 0;
        if (xSemaphoreTake(g_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            t0 = lv_tick_get();
            lv_timer_handler();
            xSemaphoreGive(g_lvgl_mutex);
        }
        uint32_t dt = lv_tick_elaps(t0);
        if (dt > 200) {
            ESP_LOGW(TAG, "lv_timer_handler slow: %u ms", (unsigned)dt);
        }
        count++;
        if (count % 100 == 0) {
            ui_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

lv_display_t* lvgl_adapter_get_display(void)
{
    return g_disp;
}

/* Force a full-screen render from the main task. Serialises against
 * lvgl_task via g_lvgl_mutex so that two tasks never enter LVGL's
 * lv_timer_handler / rendering engine concurrently, which would
 * trigger an "Invalidate area is not allowed during rendering"
 * assertion inside lv_inv_area(). */
void lvgl_adapter_refr_now(void)
{
    if (xSemaphoreTake(g_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lv_timer_handler();
        xSemaphoreGive(g_lvgl_mutex);
    }
}
