// Generated from assets/music/catalog.json by tools/encode_music.py.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *id;
    const char *title;
    const char *path;
    uint32_t duration_ms;
    uint16_t order;
} music_track_t;

static const music_track_t MUSIC_CATALOG[] = {
    {"01", "歌唱祖国", "01.opus", 209040u, 1u},
    {"02", "没有共产党就没有新中国", "02.opus", 109000u, 2u},
    {"03", "粉红色的回忆", "03.opus", 229000u, 3u},
    {"04", "童年", "04.opus", 217480u, 4u},
    {"05", "陀飞轮", "05.opus", 278133u, 5u},
    {"06", "虫儿飞", "06.opus", 162890u, 6u},
};

#define MUSIC_CATALOG_COUNT (sizeof(MUSIC_CATALOG) / sizeof(MUSIC_CATALOG[0]))
