#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport-independent results. AGAIN is temporary starvation, never EOF. */
typedef enum {
    MUSIC_SOURCE_OK = 0,
    MUSIC_SOURCE_AGAIN = 1,
    MUSIC_SOURCE_EOF = 2,
    MUSIC_SOURCE_CANCELLED = -1,
    MUSIC_SOURCE_IO_ERROR = -2,
    MUSIC_SOURCE_INVALID = -3,
    MUSIC_SOURCE_TIMEOUT = -4,
    MUSIC_SOURCE_TRUNCATED = -5,
    MUSIC_SOURCE_BAD_FRAME = -6,
} music_source_result_t;

typedef struct {
    /* Return OK only with 1..capacity bytes. Other results must return zero
     * bytes. Callbacks must be bounded; a future HTTP source must read from
     * a buffer, not wait for TLS/network I/O in the audio worker. */
    music_source_result_t (*read)(void *context, uint8_t *data,
                                  size_t capacity, size_t *received);
    void (*cancel)(void *context);
    void (*close)(void *context);
} music_source_ops_t;

typedef struct {
    const music_source_ops_t *ops;
    void *context;
    bool cancelled;
} music_source_t;

/* Initialize a zero-initialized or closed source. Context remains caller-owned.
 * The owning worker serializes read/cancel/close; these are not ISR/UI APIs. */
music_source_result_t music_source_init(music_source_t *source,
                                         const music_source_ops_t *ops,
                                         void *context);
music_source_result_t music_source_read(music_source_t *source, uint8_t *data,
                                         size_t capacity, size_t *received);
void music_source_cancel(music_source_t *source);
void music_source_close(music_source_t *source);
