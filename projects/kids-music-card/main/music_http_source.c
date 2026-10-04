#include "music_http_source.h"

#include "music_http_protocol.h"
#include "music_network.h"
#include "music_stream_buffer.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define HTTP_CHUNK_BYTES 512u
/* Connecting needs room for a TLS handshake, especially on a slow link; body
 * reads must stay short so pause/stop stay responsive. The timeout is lowered
 * again after the headers are in. */
#define HTTP_CONNECT_TIMEOUT_MS 5000
#define HTTP_IO_TIMEOUT_MS 1000
#define HTTP_USER_MAX 64u
#define HTTP_PASSWORD_MAX 128u

static const char *TAG = "music_http_source";
static SemaphoreHandle_t s_tls_gate;
static SemaphoreHandle_t s_slots;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t producer;
    TaskHandle_t consumer;
    music_stream_buffer_t buffer;
    music_source_t buffered_source;
    music_http_headers_t headers;
    bool cancelled;
    bool owner_released;
    bool paused;
    bool started;
    size_t start_bytes;
    uint64_t expected_size;
    music_http_failure_t failure;
    char url[MUSIC_HTTP_URL_MAX + 1];
    char username[HTTP_USER_MAX + 1];
    char password[HTTP_PASSWORD_MAX + 1];
    uint8_t storage[];
} http_source_t;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void log_resources(const char *stage)
{
#if CONFIG_MUSIC_TEST_CONSOLE
    /* Diagnostic checkpoints only; never run a full heap walk in UI callbacks. */
    if (!heap_caps_check_integrity_all(true)) abort();
#endif
    const uint32_t capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // Minimum is since boot, not an isolated TLS-only peak. Values must be
    // compared with the offline baseline; no audio-buffer-size guarantee here.
    ESP_LOGI(TAG, "%s: internal_free=%zu minimum_since_boot=%zu largest_block=%zu stack_watermark=%u",
             stage, heap_caps_get_free_size(capabilities),
             heap_caps_get_minimum_free_size(capabilities),
             heap_caps_get_largest_free_block(capabilities),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void zero_secret(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) {
        *bytes++ = 0;
    }
}

static bool credential_valid(const char *value, size_t maximum, bool username)
{
    if (!value || !*value) {
        return false;
    }
    size_t size = 0;
    while (size <= maximum && value[size]) {
        if ((unsigned char)value[size] < 32 || (unsigned char)value[size] > 126 ||
            (username && value[size] == ':')) {
            return false;
        }
        size++;
    }
    return size <= maximum;
}

static void flags(http_source_t *context, bool *cancelled, bool *paused)
{
    xSemaphoreTake(context->lock, portMAX_DELAY);
    *cancelled = context->cancelled;
    *paused = context->paused;
    xSemaphoreGive(context->lock);
}

static music_source_result_t source_read(void *argument, uint8_t *data,
                                          size_t capacity, size_t *received)
{
    http_source_t *context = argument;
    *received = 0;
    if (xSemaphoreTake(context->lock, 0) != pdTRUE) {
        return MUSIC_SOURCE_AGAIN;
    }
    music_source_result_t result;
    if (context->cancelled) {
        result = MUSIC_SOURCE_CANCELLED;
    } else if (context->paused || (!context->started &&
               context->buffer.used < context->start_bytes &&
               context->buffer.terminal == MUSIC_SOURCE_AGAIN)) {
        result = MUSIC_SOURCE_AGAIN;
    } else {
        context->started = true;
        result = music_source_read(&context->buffered_source, data, capacity, received);
    }
    /* Signal space availability. No network operations while holding a lock. */
    xTaskNotifyGive(context->producer);
    xSemaphoreGive(context->lock);
    return result;
}

static void source_cancel(void *argument)
{
    http_source_t *context = argument;
    xSemaphoreTake(context->lock, portMAX_DELAY);
    context->cancelled = true;
    music_stream_buffer_cancel(&context->buffer);
    xTaskNotifyGive(context->producer);
    xSemaphoreGive(context->lock);
}

static void source_close(void *argument)
{
    http_source_t *context = argument;
    xSemaphoreTake(context->lock, portMAX_DELAY);
    context->cancelled = true;
    context->owner_released = true;
    music_stream_buffer_cancel(&context->buffer);
    /* Notify UNDER the lock: once released the producer may free the context.
     * The closing task must never access it again after giving this mutex. */
    xTaskNotifyGive(context->producer);
    xSemaphoreGive(context->lock);
}

static const music_source_ops_t HTTP_OPS = {
    .read = source_read,
    .cancel = source_cancel,
    .close = source_close,
};

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_source_t *context = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        (void)music_http_headers_add(&context->headers, event->header_key, event->header_value);
    }
    return ESP_OK;
}

/* The client belongs ONLY to the producer. Never cancel/close it from another
 * task: the IDF cancel_request API can also reconnect, not just interrupt. */
static esp_http_client_handle_t client_open(http_source_t *context, uint64_t offset)
{
    music_http_headers_init(&context->headers);
    const esp_http_client_config_t config = {
        .url = context->url,
        .username = context->username,
        .password = context->password,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .timeout_ms = HTTP_CONNECT_TIMEOUT_MS,
        .buffer_size = HTTP_CHUNK_BYTES,
        .buffer_size_tx = 1024,
        .event_handler = http_event,
        .user_data = context,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }
    esp_err_t error = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (error == ESP_OK) {
        error = esp_http_client_set_header(client, "Cache-Control", "no-transform");
    }
    if (error == ESP_OK && offset) {
        char range[64];
        snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", offset);
        error = esp_http_client_set_header(client, "Range", range);
    }
    if (error != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static void retire(http_source_t *context, music_source_result_t terminal,
                    music_http_failure_t failure)
{
    xSemaphoreTake(context->lock, portMAX_DELAY);
    const bool notify = !context->cancelled;
    if (notify) {
        context->failure = failure;
        (void)music_stream_buffer_finish(&context->buffer, terminal);
    }
    xSemaphoreGive(context->lock);
    if (notify) {
        xTaskNotifyGive(context->consumer);
    }
    /* A completed producer keeps its context alive until source_close releases
     * the owner. Conversely a closed source waits only in THIS worker for IO. */
    for (;;) {
        xSemaphoreTake(context->lock, portMAX_DELAY);
        const bool released = context->owner_released;
        xSemaphoreGive(context->lock);
        if (released) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    music_source_close(&context->buffered_source);
    vSemaphoreDelete(context->lock);
    zero_secret(context->password, sizeof(context->password));
    zero_secret(context->username, sizeof(context->username));
    free(context);
    xSemaphoreGive(s_slots);
    vTaskDelete(NULL);
}

static bool wait_unpaused(http_source_t *context, music_http_recovery_t *recovery)
{
    for (;;) {
        bool cancelled, paused;
        flags(context, &cancelled, &paused);
        if (cancelled) {
            return false;
        }
        if (!paused) {
            return true;
        }
        /* No additional HTTP reads while paused, and no timeout caused simply
         * by the user's pause. Ring + one 512-byte pending chunk stay bounded. */
        music_http_recovery_progress(recovery);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

static void http_worker(void *argument)
{
    http_source_t *context = argument;
    music_http_recovery_t recovery = {0};
    music_http_failure_t failure = MUSIC_HTTP_FAILURE_NETWORK;
    music_source_result_t terminal = MUSIC_SOURCE_IO_ERROR;
    uint64_t offset = 0;
    esp_http_client_handle_t client = NULL;
    bool gate_owned = false;
    uint8_t chunk[HTTP_CHUNK_BYTES];

    log_resources("producer_start");
    music_http_recovery_begin(&recovery, now_ms());
    /* Waiting for connectivity or a retiring TLS session is producer work,
     * never an audio/UI wait. Check cancellation at every bounded interval. */
    while (!gate_owned) {
        if (!wait_unpaused(context, &recovery)) {
            goto done;
        }
        music_http_recovery_begin(&recovery, now_ms());
        if (music_http_recovery_expired(&recovery, now_ms())) {
            failure = MUSIC_HTTP_FAILURE_TIMEOUT;
            terminal = MUSIC_SOURCE_TIMEOUT;
            goto done;
        }
        if (music_network_https_ready()) {
            gate_owned = xSemaphoreTake(s_tls_gate, pdMS_TO_TICKS(50)) == pdTRUE;
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        }
    }

    while (offset < context->expected_size) {
        if (!wait_unpaused(context, &recovery)) {
            goto done;
        }
        music_http_recovery_begin(&recovery, now_ms());
        if (music_http_recovery_expired(&recovery, now_ms())) {
            failure = MUSIC_HTTP_FAILURE_TIMEOUT;
            terminal = MUSIC_SOURCE_TIMEOUT;
            goto done;
        }
        if (!music_network_https_ready()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }
        client = client_open(context, offset);
        if (!client) {
            goto done; /* Allocation/config failure is not a network retry. */
        }
        if (esp_http_client_open(client, 0) != ESP_OK ||
            esp_http_client_fetch_headers(client) < 0) {
            int certificate_flags = 0;
            (void)esp_http_client_get_and_clear_last_tls_error(client, NULL, &certificate_flags);
            if (certificate_flags) {
                failure = MUSIC_HTTP_FAILURE_CERTIFICATE;
                goto done;
            }
            goto retry;
        }
        bool cancelled, paused;
        flags(context, &cancelled, &paused);
        if (cancelled) {
            goto done;
        }
        if (music_http_recovery_expired(&recovery, now_ms())) {
            failure = MUSIC_HTTP_FAILURE_TIMEOUT;
            terminal = MUSIC_SOURCE_TIMEOUT;
            goto done;
        }
        if (esp_http_client_is_chunked_response(client)) {
            context->headers.invalid = true;
        }
        (void)esp_http_client_set_timeout_ms(client, HTTP_IO_TIMEOUT_MS);
        const int status = esp_http_client_get_status_code(client);
        const music_http_action_t action = music_http_validate_response(
            status, &context->headers, offset, context->expected_size);
        if (action == MUSIC_HTTP_RETRY) {
            goto retry;
        }
        if (action != MUSIC_HTTP_ACCEPT) {
            failure = action == MUSIC_HTTP_RESTART_REQUIRED ? MUSIC_HTTP_FAILURE_RESTART_REQUIRED :
                (status == 401 || status == 403 ? MUSIC_HTTP_FAILURE_AUTH :
                 (status == 404 ? MUSIC_HTTP_FAILURE_NOT_FOUND : MUSIC_HTTP_FAILURE_PROTOCOL));
            goto done; /* No body bytes from a rejected response enter the ring. */
        }

        log_resources("headers_checked");
        while (offset < context->expected_size) {
            if (!wait_unpaused(context, &recovery)) {
                goto done;
            }
            if (music_http_recovery_expired(&recovery, now_ms())) {
                failure = MUSIC_HTTP_FAILURE_TIMEOUT;
                terminal = MUSIC_SOURCE_TIMEOUT;
                goto done;
            }
            xSemaphoreTake(context->lock, portMAX_DELAY);
            size_t available = music_stream_buffer_space(&context->buffer);
            xSemaphoreGive(context->lock);
            if (!available) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            size_t requested = available < sizeof(chunk) ? available : sizeof(chunk);
            if (context->expected_size - offset < requested) {
                requested = (size_t)(context->expected_size - offset);
            }
            const int received = esp_http_client_read(client, (char *)chunk, (int)requested);
            if (received <= 0) {
                log_resources("read_failed");
                goto retry; /* 0 before expected size is premature EOF, not success. */
            }
            if ((size_t)received > requested) {
                failure = MUSIC_HTTP_FAILURE_PROTOCOL;
                goto done;
            }
            if (!wait_unpaused(context, &recovery)) {
                goto done;
            }
            xSemaphoreTake(context->lock, portMAX_DELAY);
            if (context->cancelled) {
                xSemaphoreGive(context->lock);
                goto done;
            }
            const size_t accepted = music_stream_buffer_write(&context->buffer, chunk, (size_t)received);
            xSemaphoreGive(context->lock);
            // Wake the higher-priority audio task only AFTER unlocking, so it
            // cannot preempt this producer just to hit a contended read lock.
            xTaskNotifyGive(context->consumer);
            if (accepted != (size_t)received) {
                failure = MUSIC_HTTP_FAILURE_PROTOCOL;
                goto done; /* Consumer only frees space; a short commit is a bug. */
            }
            /* Offset tracks bytes COMMITTED to the ring. IDF-prefetched bytes
             * not delivered/committed are discarded with the old client and
             * requested again. Existing ring and decoder half-frame survive. */
            offset += accepted;
            music_http_recovery_progress(&recovery);
        }
        if (!esp_http_client_is_complete_data_received(client)) {
            failure = MUSIC_HTTP_FAILURE_PROTOCOL;
            goto done;
        }
        ESP_LOGI(TAG, "HTTP body complete: bytes=%" PRIu64, offset);
        terminal = MUSIC_SOURCE_EOF;
        failure = MUSIC_HTTP_FAILURE_NONE;
        goto done;

retry:
        esp_http_client_cleanup(client);
        client = NULL;
        if (!wait_unpaused(context, &recovery)) {
            goto done;
        }
        if (!music_http_recovery_retry(&recovery, now_ms())) {
            failure = music_http_recovery_expired(&recovery, now_ms()) ?
                MUSIC_HTTP_FAILURE_TIMEOUT : MUSIC_HTTP_FAILURE_NETWORK;
            terminal = failure == MUSIC_HTTP_FAILURE_TIMEOUT ? MUSIC_SOURCE_TIMEOUT : MUSIC_SOURCE_IO_ERROR;
            goto done;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }

done:
    if (client) {
        esp_http_client_cleanup(client);
    }
    if (gate_owned) {
        xSemaphoreGive(s_tls_gate);
    }
    log_resources("producer_end");
    zero_secret(chunk, sizeof(chunk));
    bool cancelled_at_end, paused_at_end;
    flags(context, &cancelled_at_end, &paused_at_end);
    if (!cancelled_at_end && failure != MUSIC_HTTP_FAILURE_NONE) {
        ESP_LOGW(TAG, "HTTP source ended, reason=%d", (int)failure);
    }
    retire(context, terminal, failure);
}

esp_err_t music_http_source_service_init(void)
{
    if (s_tls_gate || s_slots) {
        return ESP_ERR_INVALID_STATE;
    }
    /* IDF's HTTP_CLIENT DEBUG logs include Authorization and complete request
     * headers. Keep its runtime level at WARN BEFORE constructing any client.
     * Kconfig also caps compiled logs at INFO and requires per-tag dynamic
     * log control. Never enable HTTP_CLIENT DEBUG
     * with a real profile, even if other components use verbose logging. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    s_tls_gate = xSemaphoreCreateMutex();
    s_slots = xSemaphoreCreateCounting(2, 2);
    if (!s_tls_gate || !s_slots) {
        if (s_tls_gate) {
            vSemaphoreDelete(s_tls_gate);
        }
        if (s_slots) {
            vSemaphoreDelete(s_slots);
        }
        s_tls_gate = NULL;
        s_slots = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool music_http_service_acquire(uint32_t timeout_ms)
{
    return s_tls_gate && xSemaphoreTake(s_tls_gate, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void music_http_service_release(void)
{
    if (s_tls_gate) xSemaphoreGive(s_tls_gate);
}

esp_err_t music_http_source_open(music_source_t *source,
                                 const music_http_source_config_t *config)
{
    if (!s_tls_gate || !s_slots) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!source || source->ops || !config || !config->expected_size ||
        config->expected_size > INT64_MAX ||
        !credential_valid(config->username, HTTP_USER_MAX, true) ||
        !credential_valid(config->password, HTTP_PASSWORD_MAX, false)) {
        ESP_LOGW(TAG, "Source config rejected: size=%llu",
                 config ? (unsigned long long)config->expected_size : 0);
        return ESP_ERR_INVALID_ARG;
    }
    char url[MUSIC_HTTP_URL_MAX + 1];
    if (!music_http_build_url(config->origin, config->audio_path, url, sizeof(url))) {
        ESP_LOGW(TAG, "Source URL rejected: path_len=%u", (unsigned)strlen(config->audio_path));
        return ESP_ERR_INVALID_ARG;
    }
    const size_t capacity = config->buffer_bytes ? config->buffer_bytes : 8192;
    const size_t threshold = config->start_bytes ? config->start_bytes : 2600;
    if (capacity < 4096 || capacity > 32768 || (capacity & (capacity - 1)) || threshold > capacity) {
        ESP_LOGW(TAG, "Source buffer rejected: capacity=%u threshold=%u", (unsigned)capacity, (unsigned)threshold);
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_slots, 0) != pdTRUE) {
        ESP_LOGW(TAG, "No free HTTP source slot");
        return ESP_ERR_NO_MEM;
    }
    http_source_t *context = calloc(1, sizeof(*context) + capacity);
    if (!context) {
        log_resources("source_open_nomem");
        ESP_LOGW(TAG, "Source allocation failed: %u bytes contiguous required",
                 (unsigned)(sizeof(*context) + capacity));
        xSemaphoreGive(s_slots);
        return ESP_ERR_NO_MEM;
    }
    context->lock = xSemaphoreCreateMutex();
    if (!context->lock) {
        free(context);
        xSemaphoreGive(s_slots);
        return ESP_ERR_NO_MEM;
    }
    memcpy(context->url, url, strlen(url) + 1);
    memcpy(context->username, config->username, strlen(config->username) + 1);
    memcpy(context->password, config->password, strlen(config->password) + 1);
    context->expected_size = config->expected_size;
    context->start_bytes = config->expected_size < threshold ? (size_t)config->expected_size : threshold;
    context->consumer = xTaskGetCurrentTaskHandle();
    (void)music_stream_buffer_init(&context->buffer, context->storage, capacity);
    (void)music_stream_buffer_attach(&context->buffered_source, &context->buffer);
    if (xTaskCreate(http_worker, "music_http", 6144, context, 4, &context->producer) != pdPASS) {
        vSemaphoreDelete(context->lock);
        zero_secret(context->password, sizeof(context->password));
        free(context);
        xSemaphoreGive(s_slots);
        return ESP_ERR_NO_MEM;
    }
    /* Fresh source was validated before task creation; initialization cannot
     * fail. Context lifetime is now owned by the producer plus source owner. */
    (void)music_source_init(source, &HTTP_OPS, context);
    return ESP_OK;
}

void music_http_source_set_paused(music_source_t *source, bool paused)
{
    if (source && source->ops == &HTTP_OPS) {
        http_source_t *context = source->context;
        xSemaphoreTake(context->lock, portMAX_DELAY);
        context->paused = paused;
        xTaskNotifyGive(context->producer);
        xSemaphoreGive(context->lock);
    }
}

void music_http_source_wait(music_source_t *source)
{
    if (source && source->ops == &HTTP_OPS) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

music_http_failure_t music_http_source_failure(music_source_t *source)
{
    if (!source || source->ops != &HTTP_OPS) {
        return MUSIC_HTTP_FAILURE_NONE;
    }
    http_source_t *context = source->context;
    xSemaphoreTake(context->lock, portMAX_DELAY);
    const music_http_failure_t failure = context->failure;
    xSemaphoreGive(context->lock);
    return failure;
}
