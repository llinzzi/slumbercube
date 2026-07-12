#ifndef SSD1322_DRIVER_H
#define SSD1322_DRIVER_H

#include "driver/spi_master.h"
#include "esp_err.h"

// GPIO配置 - 通过 menuconfig 配置
#define LCD_HOST       SPI2_HOST
#define PIN_NUM_MOSI   CONFIG_PIN_MOSI
#define PIN_NUM_CLK    CONFIG_PIN_CLK
#define PIN_NUM_CS     CONFIG_PIN_CS
#define PIN_NUM_DC     CONFIG_PIN_DC
#define PIN_NUM_RST    CONFIG_PIN_RST

// 显示参数
#define LCD_H_RES      256
#define LCD_V_RES      64
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)

/**
 * @brief 初始化SSD1322驱动
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t ssd1322_init(void);

/**
 * @brief 发送命令到SSD1322
 * @param cmd 命令字节
 */
void ssd1322_send_cmd(uint8_t cmd);

/**
 * @brief 发送数据到SSD1322
 * @param data 数据字节
 */
void ssd1322_send_data(uint8_t data);

/**
 * @brief 获取SPI设备句柄
 * @return SPI设备句柄
 */
spi_device_handle_t ssd1322_get_spi_handle(void);

/**
 * @brief 关闭OLED显示
 */
void ssd1322_display_off(void);

/**
 * @brief 打开OLED显示
 */
void ssd1322_display_on(void);

/**
 * @brief 设置列/行地址窗口
 * @param col_start 起始列地址 (0x1C..0x5B, 每列=4像素)
 * @param col_end   结束列地址
 * @param row_start 起始行 (0..63)
 * @param row_end   结束行
 */
void ssd1322_set_window(uint8_t col_start, uint8_t col_end, uint8_t row_start, uint8_t row_end);

/**
 * @brief 设置对比度电流 (0xC1)，范围 0x00-0xFF
 */
void ssd1322_set_contrast(uint8_t val);

#endif // SSD1322_DRIVER_H
