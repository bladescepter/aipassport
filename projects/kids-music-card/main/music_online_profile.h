#pragma once

#include "music_http_protocol.h"
#include "esp_err.h"

/* Private NVS bootstrap for P0, not a provisioning UI. No real values belong
 * in firmware source, sdkconfig, logs or Git. Read/write only in workers. */
typedef struct {
    char origin[MUSIC_HTTP_ORIGIN_MAX + 1];
    char audio_path[MUSIC_HTTP_PATH_MAX + 1];
    char username[65];
    char password[129];
    uint64_t size_bytes;
} music_online_profile_t;

esp_err_t music_online_profile_load(music_online_profile_t *profile);
void music_online_profile_clear(music_online_profile_t *profile);
