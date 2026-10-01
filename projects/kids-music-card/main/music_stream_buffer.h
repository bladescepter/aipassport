#pragma once

#include "music_source.h"

/* Bounded compressed-byte buffer, with caller-owned memory. No task, locks,
 * allocation or network I/O here. The future network adapter MUST serialize
 * producer and consumer access with a lock and use task notifications for
 * empty/full waits; volatile is not a substitute for synchronization. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t used;
    music_source_result_t terminal;
} music_stream_buffer_t;

music_source_result_t music_stream_buffer_init(music_stream_buffer_t *buffer,
                                                uint8_t *data, size_t capacity);
size_t music_stream_buffer_space(const music_stream_buffer_t *buffer);
/* Accept only available space. The producer must retain any unwritten bytes
 * and apply backpressure, never drop them or grow another unbounded queue. */
size_t music_stream_buffer_write(music_stream_buffer_t *buffer,
                                 const uint8_t *data, size_t size);
/* EOF/error becomes visible after queued bytes drain. First terminal wins. */
bool music_stream_buffer_finish(music_stream_buffer_t *buffer,
                                 music_source_result_t terminal);
/* Cancellation discards buffered bytes, so old generations cannot leak. */
void music_stream_buffer_cancel(music_stream_buffer_t *buffer);
music_source_result_t music_stream_buffer_attach(music_source_t *source,
                                                  music_stream_buffer_t *buffer);
