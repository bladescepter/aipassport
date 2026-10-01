#include "music_frame_reader.h"

#include <string.h>

music_source_result_t music_frame_reader_init(music_frame_reader_t *reader,
                                               uint8_t *packet, size_t capacity,
                                               uint32_t timeout_ms)
{
    if (!reader || !packet || !capacity || capacity > UINT16_MAX || !timeout_ms) {
        return MUSIC_SOURCE_INVALID;
    }
    memset(reader, 0, sizeof(*reader));
    reader->packet = packet;
    reader->capacity = capacity;
    reader->timeout_ms = timeout_ms;
    return MUSIC_SOURCE_OK;
}

static music_source_result_t latch(music_frame_reader_t *reader,
                                    music_source_result_t result)
{
    if (result == MUSIC_SOURCE_EOF &&
        (reader->header_used || reader->packet_used)) {
        result = MUSIC_SOURCE_TRUNCATED;
    }
    if (result != MUSIC_SOURCE_AGAIN) {
        reader->terminal = result;
    }
    return result;
}

music_source_result_t music_frame_reader_next(music_frame_reader_t *reader,
                                               music_source_t *source,
                                               uint64_t now_ms,
                                               size_t *packet_size)
{
    if (packet_size) {
        *packet_size = 0;
    }
    if (!reader || !reader->packet || !source || !packet_size) {
        return MUSIC_SOURCE_INVALID;
    }
    if (reader->terminal != MUSIC_SOURCE_OK) {
        return reader->terminal;
    }
    if (source->cancelled) {
        return latch(reader, MUSIC_SOURCE_CANCELLED);
    }
    if (!reader->waiting) {
        reader->waiting = true;
        reader->started_ms = now_ms;
    }
    if (now_ms < reader->started_ms) {
        return latch(reader, MUSIC_SOURCE_INVALID);
    }
    if (now_ms - reader->started_ms >= reader->timeout_ms) {
        return latch(reader, MUSIC_SOURCE_TIMEOUT);
    }

    size_t received = 0;
    if (reader->header_used < sizeof(reader->header)) {
        const music_source_result_t result = music_source_read(
            source, reader->header + reader->header_used,
            sizeof(reader->header) - reader->header_used, &received);
        if (result != MUSIC_SOURCE_OK) {
            return latch(reader, result);
        }
        reader->header_used += received;
        if (reader->header_used < sizeof(reader->header)) {
            return MUSIC_SOURCE_AGAIN;
        }
        reader->packet_size = (uint16_t)reader->header[0] |
                              ((uint16_t)reader->header[1] << 8);
        if (!reader->packet_size || reader->packet_size > reader->capacity) {
            return latch(reader, MUSIC_SOURCE_BAD_FRAME);
        }
    }

    const music_source_result_t result = music_source_read(
        source, reader->packet + reader->packet_used,
        reader->packet_size - reader->packet_used, &received);
    if (result != MUSIC_SOURCE_OK) {
        return latch(reader, result);
    }
    reader->packet_used += received;
    if (reader->packet_used < reader->packet_size) {
        return MUSIC_SOURCE_AGAIN;
    }

    *packet_size = reader->packet_size;
    reader->header_used = 0;
    reader->packet_used = 0;
    reader->packet_size = 0;
    reader->waiting = false;
    return MUSIC_SOURCE_OK;
}

void music_frame_reader_resume(music_frame_reader_t *reader, uint64_t now_ms)
{
    if (reader && reader->waiting) {
        reader->started_ms = now_ms;
    }
}
