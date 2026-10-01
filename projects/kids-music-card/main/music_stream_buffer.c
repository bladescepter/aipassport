#include "music_stream_buffer.h"

#include <string.h>

music_source_result_t music_stream_buffer_init(music_stream_buffer_t *buffer,
                                                uint8_t *data, size_t capacity)
{
    if (!buffer || !data || !capacity) {
        return MUSIC_SOURCE_INVALID;
    }
    *buffer = (music_stream_buffer_t){
        .data = data,
        .capacity = capacity,
        .terminal = MUSIC_SOURCE_AGAIN,
    };
    return MUSIC_SOURCE_OK;
}

size_t music_stream_buffer_space(const music_stream_buffer_t *buffer)
{
    return buffer && buffer->data && buffer->terminal == MUSIC_SOURCE_AGAIN
           ? buffer->capacity - buffer->used : 0;
}

size_t music_stream_buffer_write(music_stream_buffer_t *buffer,
                                 const uint8_t *data, size_t size)
{
    if (!data || !size) {
        return 0;
    }
    const size_t available = music_stream_buffer_space(buffer);
    if (size > available) {
        size = available;
    }
    if (!size) {
        return 0;
    }
    const size_t tail = (buffer->head + buffer->used) % buffer->capacity;
    size_t first = buffer->capacity - tail;
    if (first > size) {
        first = size;
    }
    memcpy(buffer->data + tail, data, first);
    memcpy(buffer->data, data + first, size - first);
    buffer->used += size;
    return size;
}

bool music_stream_buffer_finish(music_stream_buffer_t *buffer,
                                 music_source_result_t terminal)
{
    if (!buffer || !buffer->data || buffer->terminal != MUSIC_SOURCE_AGAIN ||
        (terminal != MUSIC_SOURCE_EOF && terminal != MUSIC_SOURCE_IO_ERROR &&
         terminal != MUSIC_SOURCE_TIMEOUT)) {
        return false;
    }
    buffer->terminal = terminal;
    return true;
}

void music_stream_buffer_cancel(music_stream_buffer_t *buffer)
{
    if (buffer && buffer->data) {
        buffer->used = 0;
        buffer->terminal = MUSIC_SOURCE_CANCELLED;
    }
}

static music_source_result_t buffer_read(void *context, uint8_t *data,
                                          size_t capacity, size_t *received)
{
    music_stream_buffer_t *buffer = context;
    size_t size = buffer->used;
    if (!size) {
        *received = 0;
        return buffer->terminal;
    }
    if (size > capacity) {
        size = capacity;
    }
    size_t first = buffer->capacity - buffer->head;
    if (first > size) {
        first = size;
    }
    memcpy(data, buffer->data + buffer->head, first);
    memcpy(data + first, buffer->data, size - first);
    buffer->head = (buffer->head + size) % buffer->capacity;
    buffer->used -= size;
    *received = size;
    return MUSIC_SOURCE_OK;
}

static void buffer_cancel(void *context)
{
    music_stream_buffer_cancel(context);
}

static const music_source_ops_t BUFFER_OPS = {
    .read = buffer_read,
    .cancel = buffer_cancel,
    .close = buffer_cancel,
};

music_source_result_t music_stream_buffer_attach(music_source_t *source,
                                                  music_stream_buffer_t *buffer)
{
    if (!buffer || !buffer->data || !buffer->capacity) {
        return MUSIC_SOURCE_INVALID;
    }
    return music_source_init(source, &BUFFER_OPS, buffer);
}
