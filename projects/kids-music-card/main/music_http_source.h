#pragma once

#include "music_source.h"
#include "esp_err.h"

typedef struct {
    const char *origin;
    const char *audio_path; /* Immutable file at the configured origin. */
    const char *username;
    const char *password;
    uint64_t expected_size;
    size_t buffer_bytes;   /* 16384 or 32768; zero selects 16384. */
    size_t start_bytes;    /* Zero selects ~1 second (4100 bytes). */
} music_http_source_config_t;

typedef enum {
    MUSIC_HTTP_FAILURE_NONE = 0,
    MUSIC_HTTP_FAILURE_NETWORK,
    MUSIC_HTTP_FAILURE_AUTH,
    MUSIC_HTTP_FAILURE_NOT_FOUND,
    MUSIC_HTTP_FAILURE_RESTART_REQUIRED,
    MUSIC_HTTP_FAILURE_PROTOCOL,
    MUSIC_HTTP_FAILURE_TIMEOUT,
    MUSIC_HTTP_FAILURE_CERTIFICATE,
} music_http_failure_t;

/* Initialize once from app initialization. Caps live/retiring sources at two
 * and concurrent TLS clients at one. No credentials or connection here. */
esp_err_t music_http_source_service_init(void);
/* Open from the owning audio worker: validates and copies config, allocates a
 * bounded buffer, starts a producer, returns WITHOUT Wi-Fi/TLS/HTTP waits.
 * Source must be zero-initialized/closed. Its owning task must outlive it.
 * Rapid switches may return NO_MEM until an old producer has retired. */
esp_err_t music_http_source_open(music_source_t *source,
                                 const music_http_source_config_t *config);
void music_http_source_set_paused(music_source_t *source, bool paused);
/* Event-driven empty-buffer wait, maximum 20 ms so player commands get polled.
 * Called by the owning task only, not from LVGL/UI or button callbacks. */
void music_http_source_wait(music_source_t *source);
music_http_failure_t music_http_source_failure(music_source_t *source);
/* music_source_cancel/close signal the producer; close does NOT join a TLS
 * call. The producer cleans up its own client and memory after it returns.
 * No client handle is touched concurrently by the consumer. */
