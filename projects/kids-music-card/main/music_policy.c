#include "music_policy.h"
#include <stdio.h>
#include <string.h>

uint64_t music_cache_storage_cost(uint64_t bytes)
{
    if (bytes > (UINT64_MAX - 4096) / 256 * 240 - 239) return UINT64_MAX;
    return ((bytes + 239) / 240) * 256 + 4096;
}

bool music_cache_storage_fits(uint64_t total, uint64_t used, uint64_t extra)
{
    uint64_t reserve = total / 10;
    if (reserve < 65536) reserve = 65536;
    if (total <= reserve) return false;
    const uint64_t limit = total - reserve;
    return used <= limit && extra <= limit - used;
}

void music_item_remote_path(const music_item_t *item, char *path, size_t size)
{
    if (!path || !size) return;
    path[0] = 0;
    if (!item || strlen(item->sha256) != 64) return;
    snprintf(path, size, "/v1/audio/%s.opus", item->sha256);
}

bool music_cache_toggle(music_item_t *selected, int *count, const music_item_t *item)
{
    if (!selected || !count || !item || !item->id[0] ||
        *count < 0 || *count > MUSIC_CACHE_MAX) return false;
    for (int i = 0; i < *count; ++i) {
        if (!strcmp(selected[i].id, item->id)) {
            memmove(selected + i, selected + i + 1,
                    (size_t)(*count - i - 1) * sizeof(*selected));
            memset(selected + --*count, 0, sizeof(*selected));
            return true;
        }
    }
    if (*count == MUSIC_CACHE_MAX) return false;
    selected[(*count)++] = *item;
    return true;
}

int music_playlist_step(const uint16_t *order, int count, int position, int direction)
{
    if (!order || count <= 0 || count > MUSIC_REMOTE_MAX || position < 0 ||
        position >= count || (direction != 1 && direction != -1)) return -1;
    int next = (position + direction + count) % count;
    return order[next];
}
