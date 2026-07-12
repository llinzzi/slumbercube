/* Shared I2C master bus — see i2c_bus.h. */
#include "i2c_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "I2C_BUS"

static i2c_master_bus_handle_t s_bus = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_ready = false;

esp_err_t i2c_bus_init(void)
{
    if (s_ready) return ESP_OK;

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_I2C_BUS_SDA_GPIO,
        .scl_io_num = CONFIG_I2C_BUS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* External pull-ups are present on the board (PCF85063 + SHTC3
         * both need stronger than internal ~45kΩ). Keep ESP32-C3's
         * weak internal pull-ups OFF to avoid contention. */
        .flags.enable_internal_pullup = false,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed on SDA=%d SCL=%d: %s",
                 CONFIG_I2C_BUS_SDA_GPIO, CONFIG_I2C_BUS_SCL_GPIO,
                 esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "shared I2C bus ready (SDA=%d SCL=%d, %d Hz)",
             CONFIG_I2C_BUS_SDA_GPIO, CONFIG_I2C_BUS_SCL_GPIO,
             CONFIG_I2C_BUS_FREQ_HZ);
    return ESP_OK;
}

esp_err_t i2c_bus_lock(TickType_t ticks_to_wait)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    return (xSemaphoreTake(s_mutex, ticks_to_wait) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void i2c_bus_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

i2c_master_bus_handle_t i2c_bus_get(void)
{
    return s_ready ? s_bus : NULL;
}

esp_err_t i2c_bus_deinit(void)
{
    if (!s_ready) return ESP_OK;
    esp_err_t err = i2c_del_master_bus(s_bus);
    s_bus = NULL;
    s_ready = false;
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return err;
}

bool i2c_bus_is_ready(void)
{
    return s_ready;
}
