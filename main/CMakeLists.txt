set(slumbercube_sources
    "config_screen.c"
    "wifi_provisioning.c"
    "main.c"
    "ssd1322_driver.c"
    "lvgl_adapter.c"
    "ui_wrapper.c"
    "wifi.c"
    "agent_config.c"
    "clock_screen.c"
    "font_digital.c"
    "font_station.c"
    "ui/ui.c"
    "ui/screens.c"
    "audio_player_wrapper.c"
    "ble_light_client.c"
)

set(slumbercube_requires
    lvgl__lvgl driver esp_driver_usb_serial_jtag esp_netif esp_wifi
    esp_http_server lwip nvs_flash esp_http_client json mbedtls
    esp-audio-player esp_driver_i2s espressif__button esp-libhelix-mp3
    shtc3 i2c_bus pcf85063 esp_timer
)

idf_component_register(SRCS ${slumbercube_sources}
                       INCLUDE_DIRS "."
                       REQUIRES ${slumbercube_requires})

# ESP-IDF evaluates component requirements once before sdkconfig is available.
# Link the BT target only during the configuration-aware pass. When disabled,
# ble_light_client.c compiles only unreferenced stubs, which the linker drops;
# no controller/NimBLE code or Bluetooth RAM is present in the firmware.
if(CONFIG_BLE_LIGHT_ENABLE)
    target_link_libraries(${COMPONENT_LIB} PRIVATE __idf_bt)
endif()
