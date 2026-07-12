#include "ssd1322_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
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
    vTaskDelay(pdMS_TO_TICKS(15));  // Wait for VCC to stabilize (tYP ≈ 10ms)
    ssd1322_send_cmd(0xC7); ssd1322_send_data(0x01);  // Restore master contrast
    ESP_LOGI(TAG, "Display on");
}

void ssd1322_set_contrast(uint8_t val)
{
    ssd1322_send_cmd(0xC1); ssd1322_send_data(val);
}

void ssd1322_set_window(uint8_t col_start, uint8_t col_end, uint8_t row_start, uint8_t row_end)
{
    ssd1322_send_cmd(0x15);
    ssd1322_send_data(col_start);
    ssd1322_send_data(col_end);
    ssd1322_send_cmd(0x75);
    ssd1322_send_data(row_start);
    ssd1322_send_data(row_end);
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
    /* Wait ~2ms after RST high before accessing SSD1322. The datasheet tRES
     * is ~2µs, and internal oscillator startup is typically <1ms. 2ms provides
     * a safe margin without the conservatism of the original 4ms. */
    esp_rom_delay_us(2000);

    /* Immediately turn display OFF — SSD1322 defaults to Display ON after
     * hardware reset, so any delay here causes a visible white flash from
     * random GDDRAM contents. */
    ssd1322_send_cmd(0xFD); ssd1322_send_data(0x12);  // unlock
    ssd1322_send_cmd(0xAE);                            // display off

    // 初始化SSD1322寄存器
    ssd1322_send_cmd(0xB3); ssd1322_send_data(0x91);
    ssd1322_send_cmd(0xCA); ssd1322_send_data(0x3F);
    ssd1322_send_cmd(0xA2); ssd1322_send_data(0x00);
    ssd1322_send_cmd(0xA1); ssd1322_send_data(0x00);
    ssd1322_send_cmd(0xA0); ssd1322_send_data(0x14); ssd1322_send_data(0x11);
    ssd1322_send_cmd(0xAB); ssd1322_send_data(0x01);
    ssd1322_send_cmd(0xB4); ssd1322_send_data(0xA0); ssd1322_send_data(0x05); ssd1322_send_data(0xFD);
    ssd1322_send_cmd(0xC1); ssd1322_send_data(0x9F);
    ssd1322_send_cmd(0xC7); ssd1322_send_data(0x0F);
    ssd1322_send_cmd(0xB9);                              /* linear grayscale */
    ssd1322_send_cmd(0xB1); ssd1322_send_data(0xE2);
    ssd1322_send_cmd(0xD1); ssd1322_send_data(0x82 | 0x20); ssd1322_send_data(0x20);
    ssd1322_send_cmd(0xBB); ssd1322_send_data(0x1F);
    ssd1322_send_cmd(0xB6); ssd1322_send_data(0x08);
    ssd1322_send_cmd(0xBE); ssd1322_send_data(0x07);
    ssd1322_send_cmd(0xA6);
    ssd1322_send_cmd(0xA9);                              /* exit partial display */

    /* NOTE: GDDRAM clear skipped — the anti-white-flash sequence
     * (display off → render first frame → display on) already guarantees
     * that GDDRAM is fully overwritten before the panel lights up.
     * The ~8ms DMA clear at 10/20MHz SPI is pure overhead on every wake. */

    /* NOTE: 0xAF (display on) is now called AFTER first LVGL frame is rendered */
    /* ssd1322_send_cmd(0xAF); */

    ESP_LOGI(TAG, "SSD1322 initialized");

    return ESP_OK;
}
