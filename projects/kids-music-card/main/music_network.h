#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* STA-only P0 connection manager. Call start once from app initialization,
 * never from UI callbacks. This module owns the Wi-Fi driver, default STA
 * netif and SNTP service. All reconnect work occurs in its worker task. */
esp_err_t music_network_start_saved(void);
bool music_network_https_ready(void);
