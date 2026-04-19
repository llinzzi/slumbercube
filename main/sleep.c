#include "sleep.h"
#include "ssd1322_driver.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <stdbool.h>

static const char *TAG = "SLEEP";

static volatile uint32_t s_last_activity_tick = 0;

void sleep_reset_timer(void)
{
    s_last_activity_tick = xTaskGetTickCount();
}

static void sleep_task(void *arg)
{
    uint32_t last_reset = s_last_activity_tick;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        uint32_t now = xTaskGetTickCount();
        uint32_t elapsed = (now - last_reset) * portTICK_PERIOD_MS;

        if (elapsed >= SLEEP_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Inactivity timeout, entering deep sleep");
            break;
        }
    }

    // Turn off display before sleep
    ssd1322_send_cmd(0xAE);  // Display OFF
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configure GPIO3 with pull-up, active low wakeup (button pulls low)
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_WAKEUP_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Enable GPIO wakeup from deep sleep (active low = pressed = 0)
    esp_deep_sleep_enable_gpio_wakeup(1ULL << GPIO_WAKEUP_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

    esp_deep_sleep_start();
}

esp_err_t sleep_init(bool from_deep_sleep)
{
    sleep_reset_timer();

    if (!from_deep_sleep) {
        xTaskCreate(sleep_task, "sleep_task", 2048, NULL, 3, NULL);
    }

    return ESP_OK;
}
