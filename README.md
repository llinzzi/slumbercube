# Sleep Clock

基于 ESP-IDF 5.5 框架的睡眠时钟固件，驱动 256x64 SSD1322 灰度 OLED 显示屏。

## 硬件规格

### 主控
- **芯片**: ESP32-C3-MINI-1 (WiFi/Bluetooth)
- **USB**: 原生 USB（通过 DE11 ESD 保护）
- **工作电压**: 3.3V
- **输入电源**: 5V DC

### 显示
- **型号**: SSD1322
- **分辨率**: 256x64 灰度 OLED

![SSD1322 模块接口](docs/ssd1322.png)

### PCB

![PCB 正面](docs/pcb1.png)
![PCB 反面](docs/pcb2.png)

### 外设
- **音频功放**: NS4168 (I2S 输入)
- **LED 驱动**: FM116C (双路 H 桥)

### GPIO 连接

| GPIO | 功能 | 连接到 |
|------|------|--------|
| GPIO0 | 32k_XP | 32.768kHz 晶振 |
| GPIO1 | 32k_XN | 32.768kHz 晶振 |
| GPIO2 | NS_CTRL | 音频功放 NS4168 关断控制 |
| GPIO3 | KEY | 用户按键 |
| GPIO9 | BOOT | 下载/启动按键 |
| GPIO4 | I2S_SDIN | 音频功放 NS4168 数据输入 |
| GPIO5 | I2S_SCLK | 音频功放 NS4168 时钟 |
| GPIO6 | I2S_LRCLK | 音频功放 NS4168 左右声道时钟 |
| GPIO7 | SPI_SCK | SSD1322 SCLK |
| GPIO8 | SPI_DC | SSD1322 DC |
| GPIO10 | SPI_SDA | SSD1322 SDI |
| GPIO20 | SPI_RST ｜SSD1322 RST ｜
| GPIO18 | USB_D- | USB 数据线 |
| GPIO19 | USB_D+ | USB 数据线 |
| GPIO21 | IN2 | LED 驱动 FM116C IN2 |

> SPI_CS 硬件接地，无需 GPIO。

### 电源架构

```
5V DC 输入 → ME6217 LDO → 3.3V 供主控/外设
                        → VLED 供显示背光
```

## 功能特性

- LVGL v9.4.0 UI 渲染
- EEZ Studio 生成的 UI 界面
- I4 格式 (4-bit 灰度) 显示输出
- SPI 通信 (10MHz)

## 构建

```bash
# 设置目标芯片
idf.py set-target esp32c3

# 配置项目
idf.py menuconfig

# 编译
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash

# 串口监视
idf.py -p /dev/ttyUSB0 monitor
```


## 显示接口说明

SSD1322 使用 I4 格式（4-bit 灰度，2 像素/字节）。LVGL 渲染使用 L8 格式（8-bit 灰度）。`lvgl_adapter.c` 中的 `lvgl_flush_cb()` 完成 L8 到 I4 的格式转换。

---

*原理图版本 1.9 | 2026-04-13*
