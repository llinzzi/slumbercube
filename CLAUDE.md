# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# Source ESP-IDF first
. ~/esp/esp-idf/export.sh

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

> The ESP-IDF install lives at `~/esp/esp-idf/` (no version pin in the
> path — the working tree reports its own version via `idf.py --version`).
> If you need a specific version, clone to `~/esp/vX.Y.Z/esp-idf/` instead.

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
├── SHTC3 indoor T/RH read (every 10s in active loop)
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
| Indoor sensor | `components/shtc3/shtc3.c/h` | SHTC3 T/RH over I²C, 3-tier retry, optional temp offset |
| Shared I²C bus | `components/i2c_bus/i2c_bus.c/h` | Mutex-protected master bus shared by SHTC3 + PCF85063 |
| RTC | `components/pcf85063/pcf85063.c/h` | PCF85063 RTC + alarm on shared I²C bus |

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
- **I²C bus**: SDA=21, SCL=9, 400 kHz (shared by SHTC3 + PCF85063)
- **SHTC3 sensor**: `CONFIG_SHTC3_TEMP_OFFSET_TENTHS_C` — see *SHTC3 self-heating* below

Access via `CONFIG_*` macros after `idf.py menuconfig`.

## SHTC3 self-heating

The on-board SHTC3 sits on the same PCB as the LDO, the Li-ion charger
IC, the OLED panel, and the ESP32-C3 die — all of which bias the
sensor's die temperature upward. The driver also keeps the SHTC3 in
**idle mode (~200 µA)** between reads for reliable wake on the shared
I²C bus, adding a further ~0.5 °C of self-heating.

The raw-to-°C conversion is the textbook SHTC3 formula
(`175·raw/65536 − 45`), so the bias comes entirely from the
environment. To compensate without re-flashing per unit, set
`CONFIG_SHTC3_TEMP_OFFSET_TENTHS_C` in menuconfig — the value is
**tenths of °C subtracted from the raw reading**, range −100..+100.
Typical bedside-clock bias is +10 to +30 (= 1.0 °C to 3.0 °C).

Tune against a reference thermometer after the device has been
running ≥5 minutes for the PCB to reach thermal equilibrium.

## Deep sleep wakeup

ESP32-C3 uses `esp_deep_sleep_enable_gpio_wakeup()` (guarded by `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`). Before sleep: display off, RST held low, NS4168 CTRL held low (gpio_hold_en). Wake on GPIO3 low level.

## Night mode

`clock_screen_is_night_time()` checks `tm_hour >= CONFIG_NIGHT_START_HOUR || tm_hour < CONFIG_NIGHT_END_HOUR`. In night mode: 4x4 checkerboard dithering, dim contrast (0x10), hides all weather UI, shows minimal digit clock. WiFi and weather fetch are skipped.

## Font assets

Font .ttf/.png source files live in `assets/fonts/` and `assets/images/`. The compiled LVGL font arrays are in `main/font_*.c` — these are what the build system compiles. Regenerate with `lv_font_conv` (see README.md for full commands).
