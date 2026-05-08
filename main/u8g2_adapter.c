#include "u8g2_adapter.h"
#include "ssd1322_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "U8G2_ADAPTER";
static u8g2_t g_u8g2;

/* ── Custom byte callback using existing SPI handle ── */

static uint8_t u8x8_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        /* SPI already initialized by our code */
        break;

    case U8X8_MSG_BYTE_SET_DC:
        gpio_set_level(PIN_NUM_DC, arg_int);
        break;

    case U8X8_MSG_BYTE_START_TRANSFER:
    case U8X8_MSG_BYTE_END_TRANSFER:
        /* CS is hardwired to GND, no action needed */
        break;

    case U8X8_MSG_BYTE_SEND: {
        spi_transaction_t t = {
            .length = arg_int * 8,
            .tx_buffer = arg_ptr,
        };
        spi_device_polling_transmit(ssd1322_get_spi_handle(), &t);
        break;
    }

    default:
        break;
    }
    return 1;
}

/* ── Custom GPIO/delay callback ── */

static uint8_t u8x8_gpio_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_RESET:
        gpio_set_level(PIN_NUM_RST, arg_int);
        break;

    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;

    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(arg_int * 10);
        break;

    case U8X8_MSG_DELAY_100NANO:
        /* not accurate, but sufficient for display timing */
        esp_rom_delay_us(1);
        break;

    case U8X8_MSG_DELAY_NANO:
        break;

    default:
        break;
    }
    return 1;
}

/* ── Public API ── */

esp_err_t u8g2_adapter_init(void)
{
    /* Initialize SSD1322 driver (SPI bus + device + display init) */
    ESP_ERROR_CHECK(ssd1322_init());

    /* Setup u8g2 with NHD 256x64 full-buffer mode.
     * u8g2 will send its own init sequence, but the display is already
     * initialized by ssd1322_init() — sending init twice is harmless. */
    u8g2_Setup_ssd1322_nhd_256x64_f(&g_u8g2, U8G2_R2, u8x8_byte_cb, u8x8_gpio_cb);
    u8g2_InitDisplay(&g_u8g2);

    /* Override u8g2's default remap with our working value (0x14) */
    ssd1322_send_cmd(0xFD);
    ssd1322_send_cmd(0x12);  /* unlock */
    ssd1322_send_cmd(0xA0);
    ssd1322_send_cmd(0x14);  /* remap */

    /* Keep display OFF — caller turns it on after first frame is rendered */
    u8g2_SetPowerSave(&g_u8g2, 1);

    ESP_LOGI(TAG, "u8g2 adapter initialized (256x64, full buffer)");
    return ESP_OK;
}

u8g2_t *u8g2_adapter_get_handle(void)
{
    return &g_u8g2;
}

void u8g2_adapter_display_on(void)
{
    u8g2_SetPowerSave(&g_u8g2, 0);
    ESP_LOGI(TAG, "Display on");
}

void u8g2_adapter_display_off(void)
{
    u8g2_SetPowerSave(&g_u8g2, 1);
    ESP_LOGI(TAG, "Display off");
}
