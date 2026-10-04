#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    MUSIC_NET_OFFLINE, MUSIC_NET_CONNECTING, MUSIC_NET_CONNECTED,
    MUSIC_NET_READY, MUSIC_NET_PROVISIONING, MUSIC_NET_ERROR, MUSIC_NET_TIME_TIMEOUT,
} music_network_state_t;

/* All driver/NVS/server operations run in the network worker, not UI. */
esp_err_t music_network_start_saved(void);
bool music_network_https_ready(void);
music_network_state_t music_network_state(void);
bool music_network_provision(void);
bool music_network_reconnect(void);
bool music_network_cancel_provision(void);
bool music_network_scan(void);
/* scan count: -1 busy, -2 failed; snapshots contain at most 12 SSIDs. */
int music_network_scan_results(char names[12][33]);
bool music_network_save_wifi(const char *ssid, const char *password);
bool music_network_save_auth(const char *user, const char *password);
/* 0 idle, 1 saving, 2 saved, -1 failed. */
int music_network_save_status(void);
int music_network_disconnect_reason(void);
/* Copies a snapshot; AP password is displayed locally only, never logged. */
void music_network_ap_info(char *ssid, unsigned ssid_size, char *password, unsigned password_size);
