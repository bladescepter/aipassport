#include "music_network.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define NET_IP   BIT0
#define NET_TIME BIT1
static const char *TAG = "music_network";
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static TaskHandle_t s_worker;
static atomic_bool s_started;

static void zero_secret(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) {
        *bytes++ = 0;
    }
}

static void time_synced(struct timeval *time_value)
{
    (void)time_value;
    if (s_events) {
        xEventGroupSetBits(s_events, NET_TIME);
    }
}

static void network_event(void *argument, esp_event_base_t base,
                           int32_t id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, NET_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, NET_IP);
        if (s_worker) {
            xTaskNotifyGive(s_worker);
        }
    }
    /* No NVS reads, connect waits or TLS in the event loop callback. */
}

static void network_worker(void *argument)
{
    (void)argument;
    uint32_t backoff_ms = 1000;
    bool connected = false;
    for (;;) {
        if (xEventGroupGetBits(s_events) & NET_IP) {
            if (!connected) {
                (void)esp_netif_sntp_start();
                connected = true;
            }
            backoff_ms = 1000;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        connected = false;
        (void)esp_wifi_connect();
        const EventBits_t result = xEventGroupWaitBits(
            s_events, NET_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        if (!(result & NET_IP)) {
            /* Bounded attempts, then a capped background backoff. Offline
             * playback does not depend on this task's success. */
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < 30000) {
                backoff_ms *= 2;
                if (backoff_ms > 30000) {
                    backoff_ms = 30000;
                }
            }
        }
    }
}

esp_err_t music_network_start_saved(void)
{
    if (s_events) {
        return ESP_ERR_INVALID_STATE;
    }
    /* No defaults or real credentials in source. A missing profile is a
     * normal offline boot, not permission to erase NVS or format storage. */
    nvs_handle_t handle;
    esp_err_t error = nvs_open("music_net", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    char ssid[33] = {0}, password[65] = {0};
    size_t ssid_size = sizeof(ssid), password_size = sizeof(password);
    error = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (error == ESP_OK) {
        error = nvs_get_str(handle, "wifi_pass", password, &password_size);
    }
    nvs_close(handle);
    const size_t password_length = error == ESP_OK ? strlen(password) : 0;
    if (error == ESP_OK && (!ssid[0] ||
        (password_length && password_length < 8))) {
        error = ESP_ERR_INVALID_ARG;
    }
    /* P0 supports WPA2 passphrases, not a 64-character raw PSK. */
    if (error == ESP_OK && password_length > 63) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) {
        zero_secret(password, sizeof(password));
        return error;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = password_length ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    zero_secret(password, sizeof(password));

    bool driver_initialized = false, wifi_registered = false, ip_registered = false;
    bool sntp_initialized = false, driver_started = false;
    s_events = xEventGroupCreate();
    if (!s_events) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    error = esp_netif_init();
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    // Avoid the default convenience wrapper's assert/ESP_ERROR_CHECK: a
    // network initialization failure must not reboot an offline music player.
    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta = esp_netif_new(&netif_config);
    if (!s_sta) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    error = esp_netif_attach_wifi_station(s_sta);
    if (error == ESP_OK) {
        error = esp_wifi_set_default_wifi_sta_handlers();
    }
    if (error != ESP_OK) {
        goto fail;
    }
    const wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&initialization);
    if (error != ESP_OK) {
        goto fail;
    }
    driver_initialized = true;
    error = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                network_event, NULL, &s_wifi_handler);
    if (error != ESP_OK) {
        goto fail;
    }
    wifi_registered = true;
    error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                network_event, NULL, &s_ip_handler);
    if (error != ESP_OK) {
        goto fail;
    }
    ip_registered = true;
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (error != ESP_OK) {
        goto fail;
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    sntp.sync_cb = time_synced;
    error = esp_netif_sntp_init(&sntp);
    if (error != ESP_OK) {
        goto fail;
    }
    sntp_initialized = true;
    error = esp_wifi_start();
    if (error != ESP_OK) {
        goto fail;
    }
    driver_started = true;
    if (xTaskCreate(network_worker, "music_wifi", 3072, NULL, 3, &s_worker) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    zero_secret(&config, sizeof(config));
    // Publish only after initialization can no longer roll back and destroy
    // the event group. HTTP workers may already be polling readiness.
    atomic_store_explicit(&s_started, true, memory_order_release);
    ESP_LOGI(TAG, "STA background connection started");
    return ESP_OK;

fail:
    zero_secret(&config, sizeof(config));
    /* Unregister callbacks before their event group is freed. Driver shutdown
     * is initialization work, never performed in UI/audio callbacks. */
    if (ip_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
    }
    if (wifi_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
    }
    if (sntp_initialized) {
        esp_netif_sntp_deinit();
    }
    if (driver_started) {
        esp_wifi_stop();
    }
    if (driver_initialized) {
        esp_wifi_deinit();
    }
    if (s_sta) {
        esp_netif_destroy_default_wifi(s_sta);
        s_sta = NULL;
    }
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    return error;
}

bool music_network_https_ready(void)
{
    return atomic_load_explicit(&s_started, memory_order_acquire) &&
           (xEventGroupGetBits(s_events) & (NET_IP | NET_TIME)) == (NET_IP | NET_TIME) &&
           time(NULL) >= (time_t)1704067200; /* 2024-01-01; certificate dates still verified. */
}
