/* Shared I2C master bus — see i2c_bus.h. */
#include "i2c_bus.h"
#include "esp_log.h"

#define TAG "I2C_BUS"

static i2c_master_bus_handle_t s_bus = NULL;
static bool s_ready = false;

esp_err_t i2c_bus_init(void)
{
    if (s_ready) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_I2C_BUS_SDA_GPIO,
        .scl_io_num = CONFIG_I2C_BUS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
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
    return err;
}

bool i2c_bus_is_ready(void)
{
    return s_ready;
}
