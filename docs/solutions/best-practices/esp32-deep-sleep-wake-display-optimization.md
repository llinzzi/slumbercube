---
title: ESP32-C3 deep-sleep wake display speed optimization
date: 2026-07-11
category: best-practices
module: boot
problem_type: best_practice
component: tooling
severity: medium
applies_when:
  - "ESP32 deep-sleep wake feels slow (>1s from button press to visible display)"
  - "boot timing analysis shows display path blocked by non-display I2C/GPIO init"
  - "32kHz external crystal configured but not present on board"
tags: [esp32, deep-sleep, display, boot, ssd1322, pcf85063, wake-stub]
---

# ESP32-C3 Deep-Sleep Wake Display Speed Optimization

## Context

An ESP32-C3 device with an SSD1322 OLED display took ~1.8s from button press to
visible content on deep-sleep wake. The user perceived this as "the screen
lights up very late." The goal was to get it under 1s.

The wake timeline broke down as:

| Phase | Before optimization | Dominant cost |
|-------|-------------------|---------------|
| ROM + 2nd stage bootloader + system init | ~1740ms | 32kHz XTAL probe timeout (~1.6s) |
| app_main entry → display on | ~110ms | Non-display I2C/GPIO on critical path |
| **Total (button → screen)** | **~1850ms** | |

All timing measured with `esp_timer_get_time()` at key points in `app_main()`.

## Guidance

### 1. Measure before optimizing

Add fine-grained boot timing instrumentation first:

```c
int64_t t_boot = esp_timer_get_time();
// ... each init step captures a timestamp ...
ESP_LOGI(TAG, "TOTAL to display: %lld us", t_display_on - t_boot);
```

Without this, you won't know whether the bottleneck is in your code or in the
ROM/bootloader. In our case, the ROM bootloader + 2nd stage (including a 1.6s
32kHz crystal probe timeout) dominated.

### 2. Fix the 32kHz RTC clock source first

If your board has no external 32kHz crystal, ESP-IDF defaults to probing for
one anyway and times out after ~1.6s. This is the single largest boot-time
cost. Configure the internal RC oscillator:

```bash
# sdkconfig (or idf.py menuconfig → Component config → RTC Clock)
CONFIG_RTC_CLK_SRC_INT_RC=y
# CONFIG_RTC_CLK_SRC_EXT_CRYS is not set
CONFIG_ESP32C3_RTC_CLK_SRC_INT_RC=y
CONFIG_ESP_SYSTEM_RTC_EXT_XTAL=n
```

This saved **~1.2s** — the `W (1613) clk: 32 kHz XTAL not found` warning
disappeared, and `app_main` entry went from ~1727ms to ~495ms after CPU start.

### 3. Reorder init: display path first, non-display after

The key insight: only the display hardware chain needs to be on the critical
path. Everything else can run after the screen is already showing content.

**Display-critical path (must complete before `ssd1322_display_on()`):**

```
ssd1322_init() → lvgl_adapter_init() → ui_wrapper_init() → lv_refr_now() → display_on()
```

**Deferred to after display_on:**

- Button GPIO registration (`iot_button_new_gpio_device`)
- Any sensor init that isn't needed for the first frame

**Moved before LVGL init (but still on display path):**

- `wifi_set_timezone()` — instant (setenv + tzset), needed for correct
  night-mode detection during UI creation
- `pcf85063_init()` + `apply_pcf85063_time()` — I2C read, ~14ms. Must run
  before UI init because `clock_screen_show()` calls `clock_screen_is_night_time()`
  which depends on correct system time

The anti-white-flash contract must be preserved: `ssd1322_init()` sends `0xAE`
(display off), LVGL renders the first frame into GDDRAM, then `display_on()`
lights the panel.

### 4. Tighten display driver delays

SSD1322 hardware delays have conservative defaults. Check the datasheet:

- Post-RST delay: tRES is ~2µs, internal oscillator startup <1ms. The original
  4ms (matching u8g2) was cut to 2ms — still 2× margin.
- Post-register-init `vTaskDelay(5ms)`: at 100Hz FreeRTOS tick,
  `pdMS_TO_TICKS(5) = 0` — this was dead code doing nothing.
- `display_on` VCC stabilization: `vTaskDelay(15ms)` rounds to 1 tick (10ms)
  at 100Hz. Don't bother changing it; the tick granularity absorbs the
  difference.

### 5. Reduce LVGL startup wait

The original 20ms wait (`pdMS_TO_TICKS(20) = 2 ticks`) for the LVGL task to
start was conservative. At priority 3, the LVGL task gets scheduled on the
very next FreeRTOS tick. One tick (10ms) is sufficient.

### 6. Future: skip the bootloader with a wake stub

Even after all these optimizations, the ROM + 2nd stage bootloader still takes
~400ms on deep-sleep wake. The next frontier is `esp_deep_sleep_start()` with a
wake stub in RTC memory, which can skip the bootloader entirely and jump
directly to application code. This would bring total wake-to-display under
100ms.

## Why This Matters

- **User perception**: a 1.8s wake feels broken; a 104ms wake feels instant.
- **The 32kHz crystal trap**: ESP-IDF's default config assumes an external
  crystal. Every board without one silently pays a 1.6s timeout on every boot.
  This is a one-line config fix with massive impact.
- **Init ordering is the cheapest optimization**: moving code is free; reducing
  delays risks hardware instability. Always reorder before you tighten.
- **Instrumentation pays for itself**: the BOOT TIMING table added ~20 lines
  and made every subsequent optimization measurable rather than guesswork.

## When to Apply

- Any ESP32 project where deep-sleep wake latency matters
- Especially when using a display that must show content immediately on wake
- When the board lacks a 32kHz external crystal (very common on dev boards)
- Before considering hardware changes (faster SPI, different display) —
  software reordering and config fixes are cheaper and often sufficient

## Examples

**Before (simplified):**
```
app_main:
  ssd1322_init()          // 4ms post-RST
  button_init()            // ~4ms GPIO — on display path
  wifi_set_timezone()      // was on display path
  pcf85063_init() + read   // ~14ms I2C — on display path
  lvgl_adapter_init()
  vTaskDelay(20ms)         // 2 ticks
  ui_wrapper_init()
  lv_refr_now()
  display_on()             // screen visible at ~110ms (app_main only)
                            // but ~1850ms from button press (bootloader)
```

**After:**
```
app_main:
  ssd1322_init()          // 2ms post-RST
  wifi_set_timezone()      // instant — before UI for correct night mode
  pcf85063_init() + read   // ~14ms I2C — before UI for correct time
  lvgl_adapter_init()
  vTaskDelay(10ms)         // 1 tick
  ui_wrapper_init()
  lv_refr_now()
  display_on()             // screen visible at ~104ms (app_main)
                            // ~599ms from button press (after 32kHz fix)
  button_init()            // deferred — after display visible
```

## Related

- ESP-IDF documentation: [Sleep Modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/sleep_modes.html)
- SSD1322 datasheet: tRES timing and VCC stabilization (tYP)
- Branch: `perf/deep-wake-display-speed` (this optimization)
