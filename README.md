# SlumberCube 安睡小方

[![GitHub](https://img.shields.io/badge/github-llinzzi%2Fslumbercube-blue?logo=github)](https://github.com/llinzzi/slumbercube)
[![ESP32-C3](https://img.shields.io/badge/ESP32--C3-RISC--V%20single--core-red?logo=espressif)](https://www.espressif.com)
[![SSD1322](https://img.shields.io/badge/SSD1322-256×64%20OLED-orange)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

> **Slumber** = 安睡 · **Cube** = 方块 —— 床头那块陪你安稳入眠的小方块。

## 简介

基于 **ESP-IDF 5.5** 框架的床头睡眠时钟固件，驱动 **256×64 SSD1322** 灰度 OLED 显示屏。集 WiFi 自动对时、AMAP 天气、`/api/esp` 电台流媒体、I2S 音频播放、SHTC3 室内温湿度传感、按键交互和深度休眠于一体。

夜间自动切换低亮数码管模式；白天/夜间整机会深度休眠，按键或定时唤醒。

主要能力：

- 🕒 **大字时钟** — 48px digital-7 数字显示，灰度 16 级
- 🌤 **天气 + 室内温湿度** — AMAP 接口，SHTC3 传感器
- 🎵 **电台流媒体** — HTTP MP3 流，I2S → NS4168 功放
- 🌙 **夜间模式** — 4×4 抖动数码管 + 极暗对比度，22:00–6:00
- 💤 **深度休眠** — 默认凌晨 7:50 RTC 定时唤醒，按键随时唤醒
- 📐 **方正造型** — 4 层 PCB + 亚克力外壳，整机一手可握

![夜间使用场景](assets/0.jpg)

![正面](assets/f.jpg)
![反面](assets/b.jpg)

| 线框图 | 3D 渲染图 |
|:---:|:---:|
| ![线框图](assets/1.png) | ![3D 渲染](assets/2.png) |

## 硬件规格

### 主控
| 项目 | 规格 |
|------|------|
| 模组 | ESP32-C3-WROOM-02 (RISC-V 单核, WiFi/BT) |
| Flash | 4MB DIO @ 80MHz |
| RTC | 32.768kHz 晶振 |

### 显示
| 项目 | 规格 |
|------|------|
| 型号 | SSD1322 |
| 分辨率 | 256×64 灰度 OLED |
| 接口 | SPI 10MHz, I4 灰度 (16 级) |

### GPIO 连接

| GPIO | 功能 | 连接 |
|------|------|------|
| 2 | NS_CTRL | NS4168 功放关断 |
| 3 | KEY / WAKEUP | 用户按键 (短按睡眠, 长按切歌, 深度睡眠唤醒) |
| 4 | I2S_SDIN | NS4168 数据 |
| 5 | I2S_SCLK | NS4168 时钟 |
| 6 | I2S_LRCLK | NS4168 声道 |
| 7 | SPI_SCK | SSD1322 SCLK |
| 8 | SPI_DC | SSD1322 DC |
| 9 | I2C_SCL | SHTC3 温湿度传感器 |
| 10 | SPI_SDA | SSD1322 MOSI |
| 20 | SPI_RST | SSD1322 RST |
| 21 | I2C_SDA | SHTC3 温湿度传感器 |

> SPI CS 硬件接地。唤醒 GPIO 和按键共用 GPIO3。

---

## 程序启动流程

```mermaid
flowchart TD
    A[深度睡眠] -->|RTC/按键唤醒| B[esp_sleep_get_wakeup_cause]
    B --> C{唤醒源}
    C -->|RTC| D[wake=rtc]
    C -->|按键| E[wake=btn]
    C -->|冷启动| F[wake=sys]

    D --> G[GPIO 早期初始化]
    E --> G
    F --> G

    G --> H[ssd1322_init<br/>显示保持关闭]
    H --> I[按键初始化<br/>短按=睡眠, 长按=切歌]
    I --> J[wifi_set_timezone<br/>CST-8]
    J --> K[lvgl_adapter_init<br/>L8→I4 DMA]
    K --> L[clock_screen_create<br/>首帧渲染]
    L --> M[ssd1322_display_on<br/>防白闪]

    M --> N{夜间模式?}

    N -->|是| O[跳过 WiFi/天气/音频<br/>7段数码管显示]
    N -->|否| P[WiFi STA 连接]
    P --> Q[SNTP 时间同步]
    Q --> R[读取 SHTC3 传感器]
    R --> S[audio_fetch_api<br/>单次 HTTP GET /api/esp]
    S --> T[解析天气 + 电台URL]
    T --> U{有电台URL?}
    U -->|是| V[audio_play_url<br/>HTTP 流 MP3 解码]
    U -->|否| W[跳过音频]

    V --> X[主循环 3600s]
    O --> X
    W --> X

    X --> Y{每秒检查}
    Y -->|短按/超时| Z[深度睡眠]
    Y -->|长按| AA[切换播放/停止]
    Y -->|歌曲结束| AB[auto_advance<br/>请求下一首]
    Y -->|每10秒| AC[刷新 SHTC3 传感器]
    AB --> V
    AA --> V
    AC --> X
```

---

## 屏幕布局

```
y=0   ┌──────────────────────────────────────────────┐
      │ 左: 16:30 (digital-7 48px)   右: 小雨 22°(内25.3°58%)  │
y=18  │                                    22° - 30°         │
y=36  │              [ 歌曲名居中滚动 ]                          │
      └──────────────────────────────────────────────┘
                        256×64 SSD1322
```

| 区域 | 字体 | 内容 |
|------|------|------|
| 时间 | `lv_font_digital` (digital-7, 48px, 4bpp) | HH:MM |
| 天气行 | `lv_font_station` (fusion-pixel, 10px, 1bpp) | 天气文字 + 当前温度 + 室内温湿度 |
| 温度行 | `lv_font_station` | 今日最低温度 – 最高温度 |
| 歌名行 | `lv_font_station` | 歌曲名, 居中滚动 |

---

## 夜间模式

触发条件: 22:00–6:00

- 显示切换到 Canvas 7 段数码管 (12px 灰度像素, 8×8 网格抖动)
- 对比度降到 `0x01` (极暗)
- 跳过 WiFi、天气、SHTC3、音频 — 纯时钟

---

## 唤醒机制

| 唤醒源 | `?wake=` | 说明 |
|--------|----------|------|
| RTC 定时器 (默认 7:50) | `rtc` | 每天定时唤醒 |
| GPIO3 按键 | `btn` | 手动按按键唤醒 |
| 冷启动 (上电/烧录) | `sys` | 第一次启动 |

唤醒源在启动最早期通过 `esp_sleep_get_wakeup_cause()` 检测，随后拼接到 `/api/esp` URL 中。

---

## /api/esp API 规范

### 请求

```
GET http://{server}:3000/api/esp/{device_id}?wake={src}&t={temp}&h={humidity}
```

| 参数 | 类型 | 示例 | 说明 |
|------|------|------|------|
| `device_id` | path | `543204470994` | ESP32-C3 MAC 地址 (12 hex) |
| `wake` | query | `rtc` / `btn` / `sys` | 唤醒源 |
| `t` | query | `25.3` | 室内温度 °C (SHTC3, 可选) |
| `h` | query | `58` | 室内湿度 %RH (SHTC3, 可选) |

### 响应 JSON

```json
{
  "url": "http://stream.example.com/track.mp3",
  "name": "电台名称",
  "song": "当前歌曲",
  "volume": 50,
  "weather": {
    "temp": "26",
    "text": "小雨",
    "humidity": "85",
    "tempMax": "30",
    "tempMin": "22",
    "textDay": "小雨",
    "textNight": "阴"
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `url` | string | 音频流 URL, 为空字符串则不播放 |
| `name` | string | 电台/专辑名 |
| `song` | string | 当前歌曲名, 优先显示 |
| `volume` | number | 音量 0.0–1.0 或 0–100 |
| `weather.temp` | string | 当前温度 |
| `weather.text` | string | 天气描述 (晴/多云/阴/雨/雪/雾/风 等) |
| `weather.humidity` | string | 室外湿度 |
| `weather.tempMax` | string | 今日最高温 |
| `weather.tempMin` | string | 今日最低温 |
| `weather.textDay` | string | 白天天气 |
| `weather.textNight` | string | 夜间天气 |

### 响应示例 (无音乐)

```json
{
  "weather": { "temp": "26", "text": "晴", "humidity": "50", "tempMax": "32", "tempMin": "22", "textDay": "晴", "textNight": "多云" }
}
```

> `url` 缺失或为空 → 不启动音频播放, 仅显示天气。

### 请求时机

```mermaid
sequenceDiagram
    participant ESP as ESP32-C3
    participant API as /api/esp Server
    participant Stream as Audio Stream Server

    Note over ESP: 启动 (任意唤醒源)
    ESP->>API: GET /api/esp/{id}?wake=rtc&t=25.3&h=58
    API-->>ESP: { url, name, song, volume, weather }

    alt url 存在
        ESP->>Stream: HTTP GET audio stream
        Stream-->>ESP: MP3 data
        Note over ESP: I2S 播放
    else url 为空
        Note over ESP: 仅显示天气, 不播放
    end

    Note over ESP: 歌曲结束 (或卡住3秒)
    ESP->>API: GET /api/esp/{id}?wake=rtc&t=25.4&h=57
    API-->>ESP: { url, ... }
    ESP->>Stream: HTTP GET next track

    Note over ESP: 长按按键
    ESP->>API: GET /api/esp/{id}?wake=rtc&t=25.6&h=55
    API-->>ESP: { url, ... }
    ESP->>Stream: HTTP GET new track
```

> 启动阶段只发 **一次** HTTP GET: `audio_fetch_api()` 同时解析天气和电台 URL, `audio_play_url()` 判断 URL 已缓存则直接播放, 不再重复请求。

---

## 温度传感器 (SHTC3)

```mermaid
flowchart LR
    A[主循环 每10秒] --> B[shtc3_read]
    B --> C{测量成功?}
    C -->|是| D[audio_set_indoor_env<br/>缓存温度+湿度]
    D --> E[clock_screen_set_indoor_env<br/>更新屏幕显示]
    C -->|失败| F[保留上次值<br/>3次重试后放弃本周期]
```

- **芯片**: Sensirion SHTC3 (I2C, 0x70)
- **引脚**: GPIO9 (SCL), GPIO21 (SDA)
- **读取频率**: 每 10 秒
- **容错**: 每次读取尝试 3 种策略 (正常 → 软复位 → Clock Stretching), 失败后跳过本次, 10 秒后自动重试
- **数据用途**: 屏幕显示 `(内25.3°58%)` + 作为 `?t=&h=` 参数随下次 `/api/esp` 请求发送

---

## 软件架构

```mermaid
graph TB
    subgraph Entry
        MAIN[main.c]
    end

    subgraph Display
        SSD1322[ssd1322_driver.c<br/>SPI 驱动]
        LVGL_ADAPT[lvgl_adapter.c<br/>L8→I4 DMA]
        CLOCK[clock_screen.c<br/>主界面]
        FONT_D[font_digital.c<br/>时钟数字 48px]
        FONT_S[font_station.c<br/>通用中文 10px<br/>11031 CJK 全字集]
    end

    subgraph Network
        WIFI[wifi.c<br/>STA + SNTP]
        HTTP[esp_http_client<br/>HTTPS/HTTP]
        JSON[cJSON 解析]
    end

    subgraph Audio
        AUDIO_WRAP[audio_player_wrapper.c]
        MIXER[audio_mixer]
        MP3[esp-libhelix-mp3]
        I2S[esp_driver_i2s]
        NS4168[NS4168 功放]
    end

    subgraph Sensor
        SHTC3[shtc3.c<br/>I2C 温湿度]
    end

    subgraph UI_FW
        EEZ[screens.c / ui.c]
    end

    MAIN --> SSD1322
    MAIN --> LVGL_ADAPT
    MAIN --> CLOCK
    MAIN --> WIFI
    MAIN --> AUDIO_WRAP
    MAIN --> SHTC3

    LVGL_ADAPT --> SSD1322
    CLOCK --> FONT_D
    CLOCK --> FONT_S
    CLOCK --> EEZ

    AUDIO_WRAP --> HTTP
    AUDIO_WRAP --> JSON
    AUDIO_WRAP --> MIXER
    AUDIO_WRAP --> MP3
    MIXER --> I2S
    MP3 --> MIXER
    I2S --> NS4168
```

### 核心模块

| 模块 | 文件 | 说明 |
|------|------|------|
| 入口 | `main.c` | 初始化 + 主循环 + 深度睡眠 + 唤醒检测 + SHTC3 定时刷新 |
| 显示驱动 | `ssd1322_driver.c/h` | SSD1322 SPI 命令, 复位序列, 对比度控制 |
| LVGL 适配 | `lvgl_adapter.c/h` | LVGL flush callback, L8→I4 格式转换 |
| WiFi/对时 | `wifi.c/h` | STA 连接, SNTP 同步, 设备 ID (MAC) |
| 天气/电台 API | `audio_player_wrapper.c/h` | `/api/esp` HTTP + JSON 解析 + I2S 音频播放 |
| 屏幕 UI | `clock_screen.c/h` | 时间/天气/温度/歌名布局 + Canvas 绘制 |
| SHTC3 驱动 | `components/shtc3/shtc3.c/h` | I2C 传感器读取, CRC8 校验 |
| UI 框架 | `ui/screens.c, ui/styles.c, ui/ui.c` | EEZ Studio 生成 |
| 数字字体 | `font_digital.c/h` | digital-7 48px 4bpp (时钟) |
| 通用字体 | `font_station.c/h` | fusion-pixel 10px 1bpp (11031 CJK + ASCII + 标点) |

---

## 构建

```bash
# 环境
. ~/esp/esp-idf/export.sh      # 适配你的 ESP-IDF 路径

# 构建
idf.py build

# 烧录 (macOS 通常用 /dev/cu.usbmodem*)
idf.py -p /dev/cu.usbmodem1301 flash

# 串口监视
idf.py -p /dev/cu.usbmodem1301 monitor
```

### 分区表

| 分区 | 大小 | 说明 |
|------|------|------|
| bootloader | 32KB | |
| partition table | 4KB | |
| nvs | 24KB | WiFi 凭证等 |
| phy_init | 4KB | |
| factory | 4032K (3.94MB) | 单 app 分区, 最大化利用 4MB Flash |

### 字体生成

```bash
lv_font_conv --size 10 --bpp 1 --format lvgl --no-compress --lv-include lvgl.h \
  --font assets/fonts/fusion-pixel-10px-monospaced-zh_hans.ttf \
  -r 0x0020-0x007F -r 0x00A0-0x00FF -r 0x2000-0x206F \
  -r 0x3000-0x303F -r 0xFF00-0xFFEF \
  -r 0x4E00-0x9FFF -r 0x3400-0x4DBF \
  --output main/font_station.c --lv-font-name lv_font_station
```

> Unicode 范围覆盖: Basic Latin, Latin-1 Supplement, General Punctuation, CJK 标点, 全角字符, CJK 统一汉字 (GB2312/GB18030)

---

## 配置

`idf.py menuconfig` → SlumberCube Configuration

| 分类 | 选项 | 说明 |
|------|------|------|
| WiFi | SSID, 密码 | |
| Sleep | 活跃时长, 唤醒 GPIO, 闹钟时间 | 默认 3600s, GPIO3, 7:50 |
| Night | 开始/结束小时 | 默认 22→6 |
| Audio | 默认音量 | 0–100 |
| GPIO | SPI/I2S/NS4168/按键 | 默认值见上文 GPIO 连接表 |

---

## 防白闪机制

深度睡眠唤醒时, SSD1322 GDDRAM 内容随机, 如果显示过早打开会出现白闪。

**修复策略** (多层防护):

1. 睡眠前 GPIO hold 拉低 RST → SSD1322 在睡眠期间保持复位
2. `ssd1322_init()` 复位后立即发 `0xAE` → 显示关断
3. `lv_screen_load()` 在 `lv_refr_now()` 之前 → 渲染用户黑底界面, 而非 LVGL 默认白底
4. 首帧渲染完成后才调用 `ssd1322_display_on()` → GDDRAM 中已是正确内容

---

## 依赖

| 组件 | 说明 |
|------|------|
| lvgl/lvgl ^9.4 | 图形库 |
| espressif/button ^4.1 | GPIO 按键 |
| chmorgan/esp-libhelix-mp3 | MP3 解码 |
| esp-audio-player | 音频框架 (HTTP 流 + 混音器 + I2S) |
| ESP-IDF >=5.0 | 开发框架 |

---

## 硬件设计文件

| 文件 | 路径 |
|------|------|
| 原理图 (PDF) | `assets/hardware/SCH_Schematic1_9_2026-06-21.pdf` |
| Gerber 文件 (ZIP) | `assets/hardware/Gerber_PCB1_9_2026-06-21.zip` |

---

*SlumberCube 安睡小方 · 固件 v2.2 · 2026-06-27*
