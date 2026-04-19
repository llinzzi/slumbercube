#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_init_sta(void);
bool wifi_is_connected(void);

#endif // WIFI_MANAGER_H
