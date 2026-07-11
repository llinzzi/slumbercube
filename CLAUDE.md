# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# Source ESP-IDF first
. ~/esp/v5.5.2/esp-idf/export.sh

# Build
idf.py build

# Configure (WiFi, API keys, pins, night mode, sleep)
idf.py menuconfig

# Flash & monitor
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor

# Serial debug scripts
python read_serial.py      # Filtered: WEATHER_SVC / WIFI / MAIN / crashes
python read_crash.py       # Capture Guru Meditation crashes only
```

## Target

ESP32-C3 (RISC-V). If switching target (e.g., to ESP32), use `idf.py set-target <chip>` and verify partition size fits (ESP32-C3 binary ~1.37MB requires `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`).

## Architecture

```
app_main()
├── GPIO early init (hold RST/MOSI/CLK/DC low before SSD1322 init)
├── ssd1322_init()          — SPI driver, display stays OFF
├── wifi_set_timezone() + PCF85063 RTC time sync
├── lvgl_adapter_init()     — LVGL display adapter, L8→I4 conversion
├── ui_wrapper_init()       — EEZ Studio generated UI (clock_screen)
├── ssd1322_display_on()    — AFTER first frame rendered (anti-white-flash)
├── Button init (GPIO3 + left)  — deferred, after screen visible
├── WiFi STA + SNTP sync
├── Weather fetch (AMAP)
└── Main loop (600s, 1s tick) → deep sleep
```

### Key modules

| Module | Files | Purpose |
|--------|-------|---------|
| Entry | `main/main.c` | Init sequence, active loop (10min), deep sleep |
| Display driver | `main/ssd1322_driver.c/h` | SPI commands, init sequence, contrast |
| LVGL adapter | `main/lvgl_adapter.c/h` | LVGL flush callback, L8→I4 DMA transfer |
| WiFi/SNTP | `main/wifi.c/h` | STA connect, timezone, NTP sync |
| Weather API | `main/weather_service.c/h` | AMAP HTTPS client, JSON parse |
| Weather UI | `main/clock_screen.c/h` | Full-screen clock/weather/chart canvas |
| UI framework | `main/ui/` | EEZ Studio generated (screens.c, styles.c, ui.c) |
| Fonts | `main/font_digital.c/h` | digital-7 clock digits |
| Fonts | `main/font_weather.c/h` | Chinese weather descriptions |
| Icons | `main/weather_icons.c/h` | Programmatic weather icons (no image files) |
| Loading | `main/loading_img.c/h` | Splash image (PNG→C array) |

`docs/solutions/` — documented solutions to past problems (bugs, best practices, patterns), organized by category with YAML frontmatter (`module`, `tags`, `problem_type`).

### Anti-white-flash on wake

Anti-white-flash is critical. The wake sequence must guarantee GDDRAM has rendered content before display turns on:

1. `ssd1322_init()` sends `0xAE` (display off) after reset
2. `ui_wrapper_init()` → `clock_screen_create()` → renders first frame
3. **Then** `ssd1322_display_on()` — never before

In `main/ui/screens.c`: `lv_screen_load()` must be called **before** `tick_screen_main()` to avoid the default LVGL white screen being flushed.

## Configuration

All config is in `main/Kconfig.projbuild` (ESP-IDF menuconfig → SlumberCube Configuration):

- **WiFi**: SSID, password
- **Weather**: AMAP API key, city adcode
- **Night mode**: Start/end hour (default 22→6)
- **Sleep**: Active duration, wake GPIO
- **GPIO pins**: SPI (MOSI=10, CLK=7, CS=-1, DC=8, RST=20), NS4168_CTRL=2, button=3

Access via `CONFIG_*` macros after `idf.py menuconfig`.

## Deep sleep wakeup

ESP32-C3 uses `esp_deep_sleep_enable_gpio_wakeup()` (guarded by `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`). Before sleep: display off, RST held low, NS4168 CTRL held low (gpio_hold_en). Wake on GPIO3 low level.

## Night mode

`clock_screen_is_night_time()` checks `tm_hour >= CONFIG_NIGHT_START_HOUR || tm_hour < CONFIG_NIGHT_END_HOUR`. In night mode: 4x4 checkerboard dithering, dim contrast (0x10), hides all weather UI, shows minimal digit clock. WiFi and weather fetch are skipped.

## Font assets

Font .ttf/.png source files live in `assets/fonts/` and `assets/images/`. The compiled LVGL font arrays are in `main/font_*.c` — these are what the build system compiles. Regenerate with `lv_font_conv` (see README.md for full commands).
