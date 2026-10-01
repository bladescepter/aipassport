#pragma once

#include "music_source.h"
#include <stdio.h>

typedef struct {
    FILE *file;
} music_file_source_t;

/* Blocking filesystem operations: call only from the audio/storage worker.
 * Both objects must be zero-initialized or previously closed. */
music_source_result_t music_file_source_open(music_source_t *source,
                                              music_file_source_t *file,
                                              const char *path);
