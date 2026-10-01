#include "music_file_source.h"

static music_source_result_t file_read(void *context, uint8_t *data,
                                        size_t capacity, size_t *received)
{
    music_file_source_t *file = context;
    *received = fread(data, 1, capacity, file->file);
    if (*received) {
        return MUSIC_SOURCE_OK;
    }
    return ferror(file->file) ? MUSIC_SOURCE_IO_ERROR : MUSIC_SOURCE_EOF;
}

static void file_close(void *context)
{
    music_file_source_t *file = context;
    if (file->file) {
        fclose(file->file);
        file->file = NULL;
    }
}

static const music_source_ops_t FILE_OPS = {
    .read = file_read,
    .close = file_close,
};

music_source_result_t music_file_source_open(music_source_t *source,
                                              music_file_source_t *file,
                                              const char *path)
{
    if (!source || source->ops || !file || file->file || !path || !*path) {
        return MUSIC_SOURCE_INVALID;
    }
    file->file = fopen(path, "rb");
    if (!file->file) {
        return MUSIC_SOURCE_IO_ERROR;
    }
    const music_source_result_t result = music_source_init(source, &FILE_OPS, file);
    if (result != MUSIC_SOURCE_OK) {
        file_close(file);
    }
    return result;
}
