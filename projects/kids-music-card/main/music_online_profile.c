#include "music_online_profile.h"

#include "nvs.h"
#include <string.h>

void music_online_profile_clear(music_online_profile_t *profile)
{
    if (profile) {
        volatile uint8_t *bytes = (volatile uint8_t *)profile;
        for (size_t i = 0; i < sizeof(*profile); i++) {
            bytes[i] = 0;
        }
    }
}

esp_err_t music_online_profile_load(music_online_profile_t *profile)
{
    if (!profile) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(profile, 0, sizeof(*profile));
    nvs_handle_t handle;
    esp_err_t error = nvs_open("music_net", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    const struct { const char *key; char *value; size_t capacity; } fields[] = {
        {"origin", profile->origin, sizeof(profile->origin)},
        {"auth_user", profile->username, sizeof(profile->username)},
        {"auth_pass", profile->password, sizeof(profile->password)},
    };
    for (size_t i = 0; error == ESP_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t size = fields[i].capacity;
        error = nvs_get_str(handle, fields[i].key, fields[i].value, &size);
        if (error == ESP_OK && !fields[i].value[0]) {
            error = ESP_ERR_INVALID_ARG;
        }
    }
    /* Track path/size now come from the validated, pinned remote catalog.
     * Do not require the obsolete P0 single-track bootstrap keys. */
    nvs_close(handle);
    if (error != ESP_OK) {
        music_online_profile_clear(profile);
    }
    return error;
}
