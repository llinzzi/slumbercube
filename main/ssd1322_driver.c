#include "ssd1322_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SSD1322_DRV";
static spi_device_handle_t g_spi = NULL;

void ssd1322_send_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_NUM_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd
    };
    spi_device_polling_transmit(g_spi, &t);
}

void ssd1322_send_data(uint8_t data)
{
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data
    };
    spi_device_polling_transmit(g_spi, &t);
}

spi_device_handle_t ssd1322_get_spi_handle(void)
{
    return g_spi;
}

void ssd1322_display_off(void)
{
    ssd1322_send_cmd(0xAE);  // Display off
    ESP_LOGI(TAG, "Display off");
}

void ssd1322_display_on(void)
{
    /* Ramp up: set master contrast to minimum before display-on to hide any
     * VCC charge-pump transient that could cause a white flash. */
    ssd1322_send_cmd(0xC7); ssd1322_send_data(0x00);  // Master contrast = min
    ssd1322_send_cmd(0xAF);  // Display on
    vTaskDelay(pdMS_TO_TICKS(30));  // Wait for VCC to stabilize
    ssd1322_send_cmd(0xC7); ssd1322_send_data(0x01);  // Restore master contrast
    ESP_LOGI(TAG, "Display on");
}

void ssd1322_clear_display(void)
{
    /* Fill GDDRAM with zeros (all black) via SPI */
    size_t buf_size = (LCD_H_RES / 2) * LCD_V_RES;
    uint8_t *clear_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!clear_buf) {
        ESP_LOGE(TAG, "Failed to allocate clear buffer");
        return;
    }
    memset(clear_buf, 0, buf_size);

    ssd1322_send_cmd(0x15);
    ssd1322_send_data(0x1C);
    ssd1322_send_data(0x1C + 63);
    ssd1322_send_cmd(0x75);
    ssd1322_send_data(0);
    ssd1322_send_data(63);
    ssd1322_send_cmd(0x5C);

    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = {
        .length = buf_size * 8,
        .tx_buffer = clear_buf,
    };
    spi_device_polling_transmit(g_spi, &t);

    free(clear_buf);
    ESP_LOGI(TAG, "Display cleared");
}

esp_err_t ssd1322_init(void)
{
    esp_err_t ret;

    // 配置GPIO (DC + RST)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed");
        return ret;
    }
    
    // 配置SPI总线
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES,  // 256*64 = 16384字节
    };
    ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ret;
    }
    
    // 添加SPI设备
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_PIXEL_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };
    ret = spi_bus_add_device(LCD_HOST, &devcfg, &g_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed");
        return ret;
    }
    
    // 硬件复位: RES# 低脉冲 >10us
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(1));  // 保持低电平 1ms
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));  // 等待复位完成

    // 初始化SSD1322寄存器
    ssd1322_send_cmd(0xFD); ssd1322_send_data(0x12);
    ssd1322_send_cmd(0xAE);
    ssd1322_send_cmd(0xB3); ssd1322_send_data(0x91);
    ssd1322_send_cmd(0xCA); ssd1322_send_data(0x3F);
    ssd1322_send_cmd(0xA2); ssd1322_send_data(0x00);
    ssd1322_send_cmd(0xA1); ssd1322_send_data(0x00);
    ssd1322_send_cmd(0xA0); ssd1322_send_data(0x14); ssd1322_send_data(0x11);
    ssd1322_send_cmd(0xAB); ssd1322_send_data(0x01);
    ssd1322_send_cmd(0xB4); ssd1322_send_data(0xA0); ssd1322_send_data(0xFD);
    ssd1322_send_cmd(0xC1); ssd1322_send_data(0x80);
    ssd1322_send_cmd(0xC7); ssd1322_send_data(0x01);
    ssd1322_send_cmd(0xB1); ssd1322_send_data(0xE2);
    ssd1322_send_cmd(0xD1); ssd1322_send_data(0x82); ssd1322_send_data(0x20);
    ssd1322_send_cmd(0xBB); ssd1322_send_data(0x1F);
    ssd1322_send_cmd(0xB6); ssd1322_send_data(0x08);
    ssd1322_send_cmd(0xBE); ssd1322_send_data(0x07);
    ssd1322_send_cmd(0xA6);

    /* Clear GDDRAM before turning on display to avoid white flash on wake */
    ssd1322_clear_display();

    /* NOTE: 0xAF (display on) is now called AFTER first LVGL frame is rendered */
    /* ssd1322_send_cmd(0xAF); */

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "SSD1322 initialized");
    
    return ESP_OK;
}
