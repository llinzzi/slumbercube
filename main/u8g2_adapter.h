#pragma once

#include "u8g2.h"
#include "esp_err.h"

esp_err_t u8g2_adapter_init(void);
u8g2_t *u8g2_adapter_get_handle(void);
void u8g2_adapter_display_on(void);
void u8g2_adapter_display_off(void);
