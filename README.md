# Sleep Clock

基于 ESP-IDF 5.5 框架的睡眠时钟固件，驱动 256x64 SSD1322 灰度 OLED 显示屏。支持 WiFi 自动对时、天气信息获取、按键交互和深度睡眠。

![产品照片](image_001.jpg)

## 功能特性

- **数字时钟** — 使用 digital-7 字体的超大时间显示 (HH:MM)
- **天气显示** — 通过高德天气 API 获取实时天气及温度，包含程序化绘制的天气图标（晴/阴/雨/雪/雾/风）
- **NTP 自动校时** — 上电自动连接 WiFi 并同步时间
- **日进度条** — 底部显示当日时间进度，带四等分刻度标记
- **按键交互** — 短按刷新天气，长按重新同步 NTP 并刷新天气
- **深度睡眠** — 10 分钟无操作后自动进入深度睡眠，按键唤醒
- **防白闪启动** — 上电时等待 LVGL 渲染首帧后再开启显示，避免 OLED 白闪
- **LVGL v9.4.0** — 基于 LVGL 图形库渲染，EEZ Studio 生成 UI 框架
- **I4 灰度输出** — 4-bit 灰度 (16 级)，2 像素/字节高效传输

## 硬件规格

### 主控
| 项目 | 规格 |
|------|------|
| 芯片 | ESP32-C3-MINI-1 (RISC-V 单核, WiFi/BT) |
| USB | 原生 USB（DE11 ESD 保护） |
| 工作电压 | 3.3V |
| 输入电源 | 5V DC |
| Flash | 4MB DIO @ 80MHz |
| RTC 晶振 | 32.768kHz |

### 显示
| 项目 | 规格 |
|------|------|
| 型号 | SSD1322 |
| 分辨率 | 256x64 灰度 OLED |
| 接口 | SPI (10MHz) |
| 灰度格式 | I4 (4-bit, 16 级灰度) |

### 外设
- **音频功放**: NS4168 (I2S 输入)
- **LED 驱动**: FM116C (双路 H 桥)
- **电源**: ME6217 LDO (5V → 3.3V)

### GPIO 连接

| GPIO | 功能 | 连接 |
|------|------|------|
| GPIO0 | 32k_XP | 32.768kHz 晶振 |
| GPIO1 | 32k_XN | 32.768kHz 晶振 |
| GPIO2 | NS_CTRL | NS4168 关断控制 |
| GPIO3 | KEY | 用户按键（唤醒） |
| GPIO4 | I2S_SDIN | NS4168 数据输入 |
| GPIO5 | I2S_SCLK | NS4168 时钟 |
| GPIO6 | I2S_LRCLK | NS4168 左右声道时钟 |
| GPIO7 | SPI_SCK | SSD1322 SCLK |
| GPIO8 | SPI_DC | SSD1322 DC |
| GPIO9 | BOOT | 下载/启动按键 |
| GPIO10 | SPI_SDA | SSD1322 SDI (MOSI) |
| GPIO18 | USB_D- | USB 数据线 |
| GPIO19 | USB_D+ | USB 数据线 |
| GPIO20 | SPI_RST | SSD1322 RST |
| GPIO21 | IN2 | FM116C IN2 |

> SPI_CS 硬件接地，无需占用 GPIO。

### 电源架构

```
5V DC ──► ME6217 LDO ──► 3.3V (主控/外设)
                    └──► VLED (显示)
```

## 软件架构

```
app_main()
├── GPIO/外设初始化
├── SSD1322 驱动初始化 (显示保持关闭)
├── 按键初始化 (GPIO3)
├── 时区设置 (CST-8 / UTC+8)
├── LVGL 适配器初始化
│   └── lvgl_task (FreeRTOS, 每 10ms 调用 lv_timer_handler)
├── UI 初始化 (EEZ Studio 生成)
│   └── weather_chart_create() 创建主界面
├── 显示开启 (等待首帧渲染完毕)
├── WiFi STA 连接 + SNTP 时间同步
├── 首次天气数据获取
└── 主循环 (600 秒, 1 秒间隔)
    ├── 检查短按 → 刷新天气
    ├── 检查长按 → NTP 重同步 + 刷新天气
    └── 超时 → 关闭显示 → 深度睡眠
```

### 核心模块

| 模块 | 文件 | 说明 |
|------|------|------|
| 入口 | `main.c` | 初始化流程和主循环、深度睡眠控制 |
| 显示驱动 | `ssd1322_driver.c/h` | SSD1322 SPI 驱动，含上电防白闪序列 |
| LVGL 适配 | `lvgl_adapter.c/h` | LVGL 显示适配，L8→I4 格式转换，DMA 刷新 |
| WiFi/对时 | `wifi.c/h` | WiFi STA 连接、SNTP 时间同步、时区设置 |
| 天气服务 | `weather_service.c/h` | 高德天气 API 客户端，JSON 解析 |
| 天气图表 | `weather_chart.c/h` | 全屏 UI 组件：时间、日期、天气图标、温度、进度条 |
| 字库 | `font_digital.c/h` | digital-7 等宽字体（时钟数字） |
| 字库 | `font_weather.c/h` | 天气信息字体 |
| 解压缩 | `miniz.c/h` | miniz 库，用于字体数据解压缩 |
| UI 框架 | `ui/` | EEZ Studio 生成的 LVGL UI 代码 |

## 构建与烧录

### 环境要求

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5 或更高版本
- 配置好 ESP32-C3 工具链

### 命令

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

### 预编译固件

`builds/` 目录下包含预编译好的二进制文件，可使用以下命令直接烧录：

```bash
esptool.py -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 builds/init_bootloader.bin \
  0x8000 builds/init_partition-table.bin \
  0x10000 builds/init_project-name.bin
```

### 串口调试

```bash
# 查看完整输出
idf.py -p /dev/ttyUSB0 monitor

# 或使用 Python 脚本（过滤关键日志）
python read_serial.py           # 仅显示 WEATHER_SVC / WIFI / MAIN / 崩溃信息
python read_serial2.py          # 完整输出
python read_serial3.py          # 展开输出（60 秒超时）
python read_crash.py            # 专门捕捉 Guru Meditation 崩溃
```

## 配置说明

WiFi 和天气 API 相关参数目前硬编码在源码中：

| 参数 | 位置 | 说明 |
|------|------|------|
| WiFi SSID/密码 | `wifi.c` | 目标 WiFi 网络 |
| 天气 API Key | `weather_service.c` | 高德天气 API 密钥 |
| 城市编码 | `weather_service.c` | 330100（杭州） |

如需适配不同网络或城市，请修改对应源码后重新编译。

## 功耗说明

- **工作状态**: 显示开启，持续运行约 10 分钟后自动休眠
- **深度睡眠**: 关闭显示，GPIO3 低电平唤醒，功耗极低
- **唤醒**: 短按按键即可唤醒设备

## 字体

项目使用两个编译后的 LVGL 字体，分别对应时间显示和天气信息。

### 数字字体 (`font_digital.c`)

- **源文件**: `main/digital-7 (mono).ttf`
- **用途**: 时钟时间显示 (HH:MM)
- **字符子集**: `0123456789:` （数字+冒号）
- **编译参数**: size=40, bpp=4

编译命令：

```bash
lv_font_conv --size 40 --bpp 4 --format lvgl --no-compress \
  --font "main/digital-7 (mono).ttf" \
  --symbols "0123456789:" \
  --output font_digital.c --lv-font-name lv_font_digital
```

### 天气字体 (`font_weather.c`)

- **源文件**: `main/fusion-pixel-12px-monospaced-zh_hans.ttf`
- **用途**: 天气文字、日期、温度显示
- **字符子集**: `0123456789°/-:AMPM严中云伴冰冷冻劲卷和多大天夹小少尘带平并度强微扬晴暴有未极毛气沙浓浮清烈热爆特狂疾知端细重间阴阵降雨雪雷雹雾霾静风飓龙转到`
- **编译参数**: size=12, bpp=1

编译命令：

```bash
lv_font_conv --size 12 --bpp 1 --format lvgl --no-compress \
  --font fusion-pixel-12px-monospaced-zh_hans.ttf \
  --symbols "0123456789°/-:AMPM严中云伴冰冷冻劲卷和多大天夹小少尘带平并度强微扬晴暴有未极毛气沙浓浮清烈热爆特狂疾知端细重间阴阵降雨雪雷雹雾霾静风飓龙转到" \
  --output font_weather.c --lv-font-name lv_font_weather
```

> 如需增删天气描述文字，需同步更新 `--symbols` 中的字符子集并重新编译字体。

### 未使用的字体文件

下列字体文件已下载但未在代码中引用，仅作备存：

| 文件 | 说明 |
|------|------|
| `fusion-pixel-12px-monospaced-latin.ttf` | 同款字体的拉丁字母版本 |
| `fusion-pixel-12px-monospaced-zh_hant.ttf.woff2` | 同款字体的繁体中文版本 (WOFF2) |
| `NotoSansSC-Light.ttf` | 思源黑体 Light 字重 |

## 依赖组件

| 组件 | 版本 | 说明 |
|------|------|------|
| lvgl/lvgl | ^9.4.0 | 图形用户界面库 |
| espressif/button | ^4.1.6 | GPIO 按键驱动 |
| ESP-IDF | >=5.0 | ESP32 开发框架 |

---

*原理图版本 1.9 | 2026-04-13*
