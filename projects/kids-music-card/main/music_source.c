#include "music_source.h"

music_source_result_t music_source_init(music_source_t *source,
                                         const music_source_ops_t *ops,
                                         void *context)
{
    if (!source || source->ops || !ops || !ops->read) {
        return MUSIC_SOURCE_INVALID;
    }
    source->ops = ops;
    source->context = context;
    source->cancelled = false;
    return MUSIC_SOURCE_OK;
}

music_source_result_t music_source_read(music_source_t *source, uint8_t *data,
                                         size_t capacity, size_t *received)
{
    if (received) {
        *received = 0;
    }
    if (!source || !source->ops || !data || !capacity || !received) {
        return MUSIC_SOURCE_INVALID;
    }
    if (source->cancelled) {
        return MUSIC_SOURCE_CANCELLED;
    }
    const music_source_result_t result = source->ops->read(
        source->context, data, capacity, received);
    if (*received > capacity ||
        (result == MUSIC_SOURCE_OK && *received == 0) ||
        (result != MUSIC_SOURCE_OK && *received != 0) ||
        result > MUSIC_SOURCE_EOF || result < MUSIC_SOURCE_BAD_FRAME) {
        *received = 0;
        return MUSIC_SOURCE_INVALID;
    }
    return result;
}

void music_source_cancel(music_source_t *source)
{
    if (source && source->ops && !source->cancelled) {
        source->cancelled = true;
        if (source->ops->cancel) {
            source->ops->cancel(source->context);
        }
    }
}

void music_source_close(music_source_t *source)
{
    if (source && source->ops) {
        if (source->ops->close) {
            source->ops->close(source->context);
        }
        source->ops = NULL;
        source->context = NULL;
        source->cancelled = false;
    }
}
