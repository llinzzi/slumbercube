/* PCF85063ATT/AJ real-time clock driver.
 * NXP PCF85063 — I2C address 0x51.
 * Reference: PCF85063 datasheet (rev 3.2, 2020-09-09).
 */
#include "pcf85063.h"
#include "pcf85063_internal.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "i2c_bus.h"
#include <string.h>
#include <time.h>

#define TAG "PCF85063"

/* BCD helpers live in pcf85063_bcd.c (host-testable). */
#define PCF85063_ADDR CONFIG_PCF85063_I2C_ADDR        /* default 0x51 */

/* Register map (datasheet §7) */
#define REG_CTRL1        0x00
#define REG_CTRL2        0x01
#define REG_OFFSET       0x02
#define REG_RAM_BYTE     0x03
#define REG_SECONDS      0x04
#define REG_MINUTES      0x05
#define REG_HOURS        0x06
#define REG_DAYS         0x07
#define REG_WEEKDAYS     0x08
#define REG_MONTHS       0x09
#define REG_YEARS        0x0A
#define REG_SECOND_ALARM 0x0B
#define REG_MINUTE_ALARM 0x0C
#define REG_HOUR_ALARM   0x0D
#define REG_DAY_ALARM    0x0E
#define REG_WDAY_ALARM   0x0F

/* Control_2 bits */
#define CTRL2_AIE  0x80  /* alarm interrupt enable */
#define CTRL2_AF   0x40  /* alarm flag (read, write 0 to clear) */
#define CTRL2_MI   0x20  /* minute interrupt */
#define CTRL2_HMI  0x10  /* half-minute interrupt */
#define CTRL2_TF   0x08  /* timer flag */

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_present = false;

/* ---------- Low-level I2C ---------- */

static esp_err_t read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 200);
}

static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + 8];
    if (len > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 200);
}

/* ---------- Public API ---------- */

esp_err_t pcf85063_init(void)
{
    if (s_present) return ESP_OK;

    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_master_bus_handle_t bus = i2c_bus_get();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = CONFIG_I2C_BUS_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Probe: read the seconds register. Any ACK + valid data means the
     * chip is on the bus. The seconds register's low nibble is always
     * 0-9 in BCD — if the top nibble is >9 the chip isn't responding. */
    uint8_t sec;
    err = read_regs(REG_SECONDS, &sec, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe failed (no ACK on 0x%02X): %s",
                 PCF85063_ADDR, esp_err_to_name(err));
        goto fail;
    }
    if (((sec >> 4) & 0x0Fu) > 9u) {
        ESP_LOGW(TAG, "probe returned garbage 0x%02X, chip likely absent", sec);
        goto fail;
    }

    /* Put the chip in 24h mode (bit 12_24 in CTRL1 = 0) and clear the
     * alarm/INT settings we don't want at boot. Leave CAP_SEL = 0
     * (default 7 pF) — matches our 12.5 pF load crystal. */
    uint8_t ctrl1 = 0x00;
    err = write_regs(REG_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) goto fail;

    /* Clear Control_2: no alarm IRQ, no minute IRQ, clear AF and TF. */
    uint8_t ctrl2 = 0x00;
    err = write_regs(REG_CTRL2, &ctrl2, 1);
    if (err != ESP_OK) goto fail;

    s_present = true;
    ESP_LOGI(TAG, "PCF85063 detected at 0x%02X on shared I2C bus",
             PCF85063_ADDR);
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pcf85063_deinit(void)
{
    if (!s_present) return ESP_OK;
    esp_err_t err = i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    if (err != ESP_OK) return err;
    /* Bus is owned by i2c_bus — leave it alone. */
    s_present = false;
    return ESP_OK;
}

bool pcf85063_is_present(void)
{
    return s_present;
}

esp_err_t pcf85063_read_datetime(pcf85063_datetime_t *dt)
{
    if (!dt) return ESP_ERR_INVALID_ARG;
    if (!s_present) return ESP_ERR_INVALID_STATE;

    uint8_t buf[7];
    esp_err_t err = read_regs(REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    /* Per datasheet, VL (low-voltage) bit lives in SECONDS bit 7. If
     * it's set, the integrity of the clock is suspect (battery/oscillator
     * dropout). Still report what we read — caller can decide what to do. */
    dt->second = pcf85063_from_bcd((uint8_t)(buf[0] & 0x7Fu));
    dt->minute = pcf85063_from_bcd((uint8_t)(buf[1] & 0x7Fu));
    dt->hour   = pcf85063_from_bcd((uint8_t)(buf[2] & 0x3Fu));
    dt->day    = pcf85063_from_bcd((uint8_t)(buf[3] & 0x3Fu));
    dt->weekday= (uint8_t)(buf[4] & 0x07u);
    dt->month  = pcf85063_from_bcd((uint8_t)(buf[5] & 0x1Fu));
    dt->year   = (uint16_t)(2000u + pcf85063_from_bcd(buf[6]));

    return ESP_OK;
}

esp_err_t pcf85063_set_datetime(const pcf85063_datetime_t *dt)
{
    if (!dt) return ESP_ERR_INVALID_ARG;
    if (!s_present) return ESP_ERR_INVALID_STATE;

    /* Mask off OS (oscillator) bit in SECONDS — we always want the
     * oscillator running, so clear bit 7. */
    uint8_t buf[7];
    buf[0] = pcf85063_to_bcd(dt->second) & 0x7Fu;  /* OS=0 */
    buf[1] = pcf85063_to_bcd(dt->minute) & 0x7Fu;
    buf[2] = pcf85063_to_bcd(dt->hour)   & 0x3Fu;
    buf[3] = pcf85063_to_bcd(dt->day)    & 0x3Fu;
    buf[4] = (uint8_t)(dt->weekday & 0x07u);
    buf[5] = pcf85063_to_bcd(dt->month)  & 0x1Fu;
    buf[6] = pcf85063_to_bcd((uint8_t)(dt->year % 100u));

    return write_regs(REG_SECONDS, buf, sizeof(buf));
}

esp_err_t pcf85063_set_alarm(const pcf85063_alarm_t *alarm)
{
    if (!alarm) return ESP_ERR_INVALID_ARG;
    if (!s_present) return ESP_ERR_INVALID_STATE;

    /* The alarm registers use bit 7 = "disable this field" (don't-care).
     * If alarm->enable is false, all four fields are forced to wildcard. */
    uint8_t buf[4];
    if (alarm->enable) {
        buf[0] = pcf85063_alarm_field_disabled(alarm->minute) ?
                 (uint8_t)0x80u : (uint8_t)(pcf85063_to_bcd(alarm->minute) & 0x7Fu);
        buf[1] = pcf85063_alarm_field_disabled(alarm->hour) ?
                 (uint8_t)0x80u : (uint8_t)(pcf85063_to_bcd(alarm->hour) & 0x3Fu);
        buf[2] = pcf85063_alarm_field_disabled(alarm->day) ?
                 (uint8_t)0x80u : (uint8_t)(pcf85063_to_bcd(alarm->day) & 0x3Fu);
        buf[3] = pcf85063_alarm_field_disabled(alarm->weekday) ?
                 (uint8_t)0x80u : (uint8_t)(alarm->weekday & 0x07u);
    } else {
        buf[0] = 0x80; buf[1] = 0x80; buf[2] = 0x80; buf[3] = 0x80;
    }

    esp_err_t err = write_regs(REG_SECOND_ALARM, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    /* Clear any leftover AF from a prior match. */
    return pcf85063_clear_alarm_flag();
}

esp_err_t pcf85063_clear_alarm_flag(void)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    uint8_t ctrl2;
    esp_err_t err = read_regs(REG_CTRL2, &ctrl2, 1);
    if (err != ESP_OK) return err;
    /* AF (bit 6) and TF (bit 3) are clear-on-write-0; preserve the rest. */
    uint8_t new_ctrl2 = (uint8_t)(ctrl2 & ~(CTRL2_AF | CTRL2_TF));
    if (new_ctrl2 == ctrl2) return ESP_OK;  /* nothing to clear */
    return write_regs(REG_CTRL2, &new_ctrl2, 1);
}

esp_err_t pcf85063_enable_alarm_int(bool enable)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    uint8_t ctrl2;
    esp_err_t err = read_regs(REG_CTRL2, &ctrl2, 1);
    if (err != ESP_OK) return err;

    uint8_t want = (uint8_t)(enable ? (ctrl2 | CTRL2_AIE)
                                    : (ctrl2 & ~CTRL2_AIE));
    if (want == ctrl2) return ESP_OK;
    return write_regs(REG_CTRL2, &want, 1);
}

esp_err_t pcf85063_sync_from_system(void)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;

    /* Read system time as UTC. The TZ env was set by wifi_set_timezone() —
     * gmtime_r() ignores TZ and treats the epoch as UTC by definition. */
    time_t now;
    time(&now);
    struct tm utc;
    gmtime_r(&now, &utc);

    /* Sanity check: refuse to write garbage that would put the RTC decades
     * in the wrong place. */
    int year = utc.tm_year + 1900;
    if (year < 2025 || year > 2099) {
        ESP_LOGW(TAG, "system year=%d out of range, skipping sync", year);
        return ESP_ERR_INVALID_ARG;
    }

    pcf85063_datetime_t dt = {
        .year    = (uint16_t)year,
        .month   = (uint8_t)(utc.tm_mon + 1),
        .day     = (uint8_t)utc.tm_mday,
        .weekday = (uint8_t)(utc.tm_wday < 0 ? 0 : utc.tm_wday),
        .hour    = (uint8_t)utc.tm_hour,
        .minute  = (uint8_t)utc.tm_min,
        .second  = (uint8_t)utc.tm_sec,
    };

    esp_err_t err = pcf85063_set_datetime(&dt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_datetime failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "synced from system time: %04u-%02u-%02u %02u:%02u:%02u UTC",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return ESP_OK;
}
