/* SHTC3 temperature & humidity sensor driver.
 * Sensirion SHTC3 — I2C address 0x70.
 * Reference: SHTC3 datasheet (rev 5, May 2023).
 */
#include "shtc3.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "SHTC3"

#define SHTC3_ADDR    0x70

/* Commands (16-bit, MSB first) */
#define CMD_WAKE         0x3517
#define CMD_SLEEP        0xB098
#define CMD_SOFT_RESET   0x805D
#define CMD_MEAS_T_RH    0x7866   /* clock stretching disabled, normal, T first */
#define CMD_MEAS_T_RH_CS 0x58E0   /* clock stretching enabled, normal, T first */
#define CMD_READ_ID      0xEFC8

/* Measurement delay: typical 8.2ms, max 12.1ms for normal mode.
 * Use 20ms to accommodate clones with slower conversion. */
#define MEAS_DELAY_MS    20

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_present = false;

/* CRC8 — polynomial 0x31, init 0xFF, no reflect, no xorout */
static uint8_t shtc3_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t read_bytes(uint8_t *out, size_t len)
{
    return i2c_master_receive(s_dev, out, len, 200);
}

/* Try a single measurement with the given command. Returns true on success. */
static bool try_measure(uint16_t meas_cmd, float *temp_c, float *humidity)
{
    /* Sensor stays in idle mode between reads (no SLEEP), so a single
     * WAKE is sufficient and harmless. 1 ms delay matches datasheet
     * wake-up time (240 μs typical). */
    if (write_cmd(CMD_WAKE) != ESP_OK) {
        ESP_LOGW(TAG, "wake cmd I2C error");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    vTaskDelay(pdMS_TO_TICKS(1));

    if (write_cmd(meas_cmd) != ESP_OK) {
        ESP_LOGW(TAG, "measure cmd 0x%04X I2C error", meas_cmd);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(MEAS_DELAY_MS));

    uint8_t buf[6];
    if (read_bytes(buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "read 6 bytes I2C error");
        return false;
    }

    if (shtc3_crc8(&buf[0], 2) != buf[2]) {
        ESP_LOGW(TAG, "T CRC mismatch: %02X%02X (CRC=%02X expected=%02X)",
                 buf[0], buf[1], shtc3_crc8(&buf[0], 2), buf[2]);
        return false;
    }
    if (shtc3_crc8(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "RH CRC mismatch: %02X%02X (CRC=%02X expected=%02X)",
                 buf[3], buf[4], shtc3_crc8(&buf[3], 2), buf[5]);
        return false;
    }

    uint16_t raw_t  = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_rh = ((uint16_t)buf[3] << 8) | buf[4];

    *temp_c   = (175.0f * raw_t)  / 65536.0f - 45.0f;
    *humidity = (100.0f * raw_rh) / 65536.0f;

    ESP_LOGD(TAG, "raw T=0x%04X RH=0x%04X → %.1f°C %.0f%%", raw_t, raw_rh, *temp_c, *humidity);
    return true;
}

bool shtc3_init(void)
{
    if (s_present) return true;

    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    i2c_master_bus_handle_t bus = i2c_bus_get();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = CONFIG_I2C_BUS_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Probe: wake + read ID. Genuine SHTC3 ID is 0x0807;
     * some variants/clones return 0x0887 or other values. */
    if (write_cmd(CMD_WAKE) != ESP_OK) {
        ESP_LOGW(TAG, "sensor absent (no ACK on wake)");
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    if (write_cmd(CMD_READ_ID) != ESP_OK) {
        ESP_LOGW(TAG, "sensor absent (no ACK on read ID)");
        goto fail;
    }
    uint8_t id_buf[3];
    if (read_bytes(id_buf, sizeof(id_buf)) != ESP_OK) {
        ESP_LOGW(TAG, "sensor absent (no ID bytes)");
        goto fail;
    }
    if (shtc3_crc8(id_buf, 2) != id_buf[2]) {
        ESP_LOGW(TAG, "ID CRC mismatch — sensor not SHTC3");
        goto fail;
    }
    uint16_t id = ((uint16_t)id_buf[0] << 8) | id_buf[1];
    ESP_LOGI(TAG, "SHTC3 detected (ID=0x%04X)", id);

    /* Leave sensor in idle mode (don't SLEEP). On shared I2C buses the
     * sleep→wake transition is unreliable — WAKE commands sometimes
     * fail to bring the sensor out of sleep. Idle current is ~200 μA,
     * negligible for the 30-min active window. */

    s_present = true;
    return true;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    /* Bus is owned by i2c_bus — leave it alive for PCF85063. */
    return false;
}

bool shtc3_read(float *temp_c, float *humidity)
{
    if (!s_present && !shtc3_init()) return false;
    if (!temp_c || !humidity) return false;

    /* Attempt 1: clock-stretching disabled (faster).
     * Leave the sensor in idle mode (don't SLEEP) so the next read
     * doesn't need WAKE — avoids the unreliable sleep→wake transition
     * on shared I2C buses. On battery power the sensor draws ~200 μA
     * idle vs ~0.5 μA sleep, acceptable for a 30-min active window. */
    if (try_measure(CMD_MEAS_T_RH, temp_c, humidity)) {
        return true;
    }

    /* Attempt 2: soft-reset the sensor, then retry with clock-stretching
     * disabled again (a clean slate often fixes intermittent CRC errors). */
    ESP_LOGW(TAG, "first attempt failed, soft-reset and retry");
    write_cmd(CMD_SOFT_RESET);  /* best-effort, resets to idle */
    vTaskDelay(pdMS_TO_TICKS(2));

    if (try_measure(CMD_MEAS_T_RH, temp_c, humidity)) {
        return true;
    }

    /* Attempt 3: clock-stretching enabled (most compatible, handles
     * slow clones and marginal I2C wiring). */
    ESP_LOGW(TAG, "second attempt failed, retry with clock-stretching");
    vTaskDelay(pdMS_TO_TICKS(10));

    if (try_measure(CMD_MEAS_T_RH_CS, temp_c, humidity)) {
        return true;
    }

    ESP_LOGW(TAG, "all 3 attempts failed — will retry next cycle");
    return false;
}