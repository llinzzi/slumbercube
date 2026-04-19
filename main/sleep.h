#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#define SLEEP_TIMEOUT_MS    20000  // 20 seconds
#define GPIO_WAKEUP_PIN     3     // GPIO3, active low (pressed = low)

esp_err_t sleep_init(bool from_deep_sleep);
void sleep_reset_timer(void);
void sleep_enter(void);

#endif // SLEEP_MANAGER_H
