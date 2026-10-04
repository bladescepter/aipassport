#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MUSIC_PAGE_SIZE 20
#define MUSIC_REMOTE_MAX 1000
#define MUSIC_CACHE_MAX 5
#define MUSIC_LOCAL_MAX 16
#define MUSIC_TITLE_BYTES 96
#define MUSIC_RELEASE_BYTES 48
#define MUSIC_LOCAL_PATH_BYTES 40
#define MUSIC_DEFAULT_ORIGIN "https://kidmusic.xiyuan.wiki"
typedef struct {
    char id[40];
    char title[MUSIC_TITLE_BYTES];
    char sha256[65];
    /* Local SPIFFS path only: /music/xx.opus or /music/c_<24>.op. Remote
     * catalog paths are derived from sha256 instead of stored, because the
     * per-item struct must stay small on this no-PSRAM device. */
    char local[MUSIC_LOCAL_PATH_BYTES];
    uint64_t size;
    int index;
    bool online;
} music_item_t;
/* Conservative SPIFFS estimate (256-byte pages, payload allowance 240).
 * Keep 10% of reported filesystem capacity free for indexes/GC. */
uint64_t music_cache_storage_cost(uint64_t bytes);
bool music_cache_storage_fits(uint64_t total, uint64_t used, uint64_t extra);
/* Writes /v1/audio/<sha256>.opus for an online item. */
void music_item_remote_path(const music_item_t *item, char *path, size_t size);
bool music_cache_toggle(music_item_t *selected, int *count, const music_item_t *item);
int music_playlist_step(const uint16_t *order, int count, int position, int direction);
