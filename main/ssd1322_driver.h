#ifndef SSD1322_DRIVER_H
#define SSD1322_DRIVER_H

#include "driver/spi_master.h"
#include "esp_err.h"

// GPIO配置
#define LCD_HOST       SPI2_HOST
#define PIN_NUM_MOSI   10  // IO10: SDA
#define PIN_NUM_CLK    7   // IO7: SCL
#define PIN_NUM_CS     -1  // IO0: CS (硬件接地)
#define PIN_NUM_DC     8   // IO8: DC
#define PIN_NUM_RST    20  // IO20: RST

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
 * @brief 清空GDDRAM（全黑），避免唤醒时闪白
 */
void ssd1322_clear_display(void);

/**
 * @brief 设置对比度电流 (0xC1)，范围 0x00-0xFF
 */
void ssd1322_set_contrast(uint8_t val);

#endif // SSD1322_DRIVER_H
