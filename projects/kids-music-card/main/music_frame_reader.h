#pragma once

#include "music_source.h"

/* Fixed-size state: no allocation and no entire-song copy. Partial headers
 * and payloads survive AGAIN. Terminal errors latch until reinitialized. */
typedef struct {
    uint8_t *packet;
    size_t capacity;
    uint8_t header[2];
    size_t header_used;
    size_t packet_used;
    uint16_t packet_size;
    uint64_t started_ms;
    uint32_t timeout_ms;
    bool waiting;
    music_source_result_t terminal;
} music_frame_reader_t;

music_source_result_t music_frame_reader_init(music_frame_reader_t *reader,
                                               uint8_t *packet, size_t capacity,
                                               uint32_t timeout_ms);
/* now_ms must be monotonic. At most two source reads per call; no busy loop.
 * OK: *packet_size is a complete frame (valid until the next call).
 * AGAIN: the caller must yield and process commands before trying again.
 * EOF: only at a clean frame boundary. All negative results are failures.
 * Timeout covers assembling one frame, including repeated partial progress.
 * Do not call while paused: rebase its deadline on resume, preserving data. */
music_source_result_t music_frame_reader_next(music_frame_reader_t *reader,
                                               music_source_t *source,
                                               uint64_t now_ms,
                                               size_t *packet_size);
void music_frame_reader_resume(music_frame_reader_t *reader, uint64_t now_ms);
