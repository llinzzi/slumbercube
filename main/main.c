#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1322_driver.h"
#include "lvgl_adapter.h"
#include "ui.h"
#include "wifi.h"
#include "weather_service.h"
#include "esp_sleep.h"
#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "MAIN";

#define ACTIVE_DURATION_SECS 600
#define WAKEUP_GPIO_NUM       3
#define BUTTON_GPIO_NUM       3

static button_handle_t g_btn = NULL;
static weather_data_t s_weather;
static volatile bool s_sleep_pending = false;

/* Lightweight: only set flag. Sleep is deferred to main loop. */
static void button_press_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Button pressed, going to sleep");
    s_sleep_pending = true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SSD1322 OLED with LVGL, active=%ds", ACTIVE_DURATION_SECS);

    /* Hold RST high immediately to prevent SSD1322 from seeing a floating RST
     * during bootloader phase (GPIO20 defaults to input, high-Z). */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << PIN_NUM_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(PIN_NUM_RST, 1);

    // Initialize SSD1322 driver first (display stays OFF until first frame rendered)
    ESP_ERROR_CHECK(ssd1322_init());

    // Initialize button (any press triggers sleep)
    button_config_t btn_cfg = {
        .short_press_time = 200,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO_NUM,
        .active_level = 0,
        .enable_power_save = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);
    if (err == ESP_OK) {
        iot_button_register_cb(g_btn, BUTTON_PRESS_DOWN, NULL, button_press_cb, NULL);
        ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO_NUM);
    } else {
        ESP_LOGE(TAG, "Failed to init button on GPIO%d: %s", BUTTON_GPIO_NUM, esp_err_to_name(err));
    }

    // Always set timezone after wake (TZ env var is lost during deep sleep)
    wifi_set_timezone();

    // Initialize LVGL before WiFi (clean heap avoids allocation failures)
    ESP_ERROR_CHECK(lvgl_adapter_init());

    // Wait for LVGL task to start
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create UI (first frame rendered and flushed to GDDRAM inside this call)
    ESP_ERROR_CHECK(ui_wrapper_init());

    // Turn on display AFTER first frame is in GDDRAM — eliminates white flash on wake
    ssd1322_display_on();

    // Pass weather data to screens if available (from a previous fetch)
    if (s_weather.valid) {
        screens_set_weather_data_ptr(&s_weather);
    }

    // Wait for UI to load
    vTaskDelay(pdMS_TO_TICKS(100));

    // Always init TCP/IP stack + start WiFi for button-press weather
    wifi_ensure_netif();
    if (wifi_init_sta() == ESP_OK) {
        if (!wifi_is_time_set()) {
            wifi_mark_time_set();
        }
    }

    // Initial weather fetch (retries handle async WiFi connection)
    for (int retry = 0; retry < 5; retry++) {
        esp_err_t err = weather_fetch(&s_weather);
        if (err == ESP_OK) {
            screens_set_weather_data_ptr(&s_weather);
            break;
        }
        ESP_LOGW(TAG, "Boot weather fetch attempt %d/5 failed", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "Running for %d seconds before sleep, button wakes", ACTIVE_DURATION_SECS);

    // Main loop: button press or timeout → sleep
    for (int i = 0; i < ACTIVE_DURATION_SECS; i++) {
        if (s_sleep_pending) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Time to sleep, turning off display");

    // Turn off display before deep sleep
    ssd1322_display_off();

    // Configure GPIO3 low-level as wakeup source
    esp_deep_sleep_enable_gpio_wakeup((1ULL << WAKEUP_GPIO_NUM), ESP_GPIO_WAKEUP_GPIO_LOW);

    ESP_LOGI(TAG, "Entering deep sleep, GPIO%d will wake on low level", WAKEUP_GPIO_NUM);

    // Enter deep sleep
    esp_deep_sleep_start();
}
