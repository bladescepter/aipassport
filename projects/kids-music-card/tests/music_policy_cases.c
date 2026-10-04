#include "music_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    music_item_t selected[MUSIC_CACHE_MAX] = {0};
    music_item_t item = {0};
    int count = 0;
    for (int i = 0; i < 5; ++i) {
        snprintf(item.id, sizeof(item.id), "song-%d", i);
        item.index = 20 + i;
        assert(music_cache_toggle(selected, &count, &item));
    }
    assert(count == 5);
    strcpy(item.id, "sixth");
    assert(!music_cache_toggle(selected, &count, &item));
    assert(count == 5);
    strcpy(item.id, "song-2");
    assert(music_cache_toggle(selected, &count, &item));
    assert(count == 4 && !strcmp(selected[2].id, "song-3"));
    strcpy(item.id, "other-page"); item.index = 77;
    assert(music_cache_toggle(selected, &count, &item));
    assert(count == 5 && selected[4].index == 77);
    strcpy(item.id, "song-0");
    assert(music_cache_toggle(selected, &count, &item));
    assert(!strcmp(selected[0].id, "song-1"));
    assert(!music_cache_toggle(NULL, &count, &item));
    assert(!music_cache_toggle(selected, NULL, &item));
    item.id[0] = 0;
    assert(!music_cache_toggle(selected, &count, &item));
    count = 6;
    assert(!music_cache_toggle(selected, &count, &item));
    uint16_t order[] = {3, 0, 2, 1};
    assert(music_playlist_step(order, 4, 0, -1) == 1);
    assert(music_playlist_step(order, 4, 3, 1) == 3);
    assert(music_playlist_step(order, 4, 1, 1) == 2);
    assert(music_playlist_step(order, 1, 0, -1) == 3);
    assert(music_playlist_step(order, 0, 0, 1) == -1);
    assert(music_playlist_step(NULL, 4, 0, 1) == -1);
    assert(music_playlist_step(order, 4, -1, 1) == -1);
    assert(music_playlist_step(order, 4, 4, 1) == -1);
    assert(music_playlist_step(order, 4, 0, 2) == -1);
    assert(music_cache_storage_cost(240) == 4352);
    assert(music_cache_storage_cost(241) == 4608);
    assert(music_cache_storage_cost(UINT64_MAX) == UINT64_MAX);
    assert(!music_cache_storage_fits(65536, 0, 1));
    assert(music_cache_storage_fits(1000000, 800000, 100000));
    assert(!music_cache_storage_fits(1000000, 800000, 100001));
    assert(!music_cache_storage_fits(1000000, UINT64_MAX, 1));
    assert(!music_cache_storage_fits(1000000, 1, UINT64_MAX));
    /* Largest five initial tracks plus both indexes, not two full generations. */
    const uint64_t sizes[] = {857146, 938982, 891750, 1140374, 667890};
    uint64_t estimate = 8192;
    for (unsigned i = 0; i < 5; ++i) estimate += music_cache_storage_cost(sizes[i]);
    assert(music_cache_storage_fits(5400000, 0, estimate));
    assert(!music_cache_storage_fits(5400000, estimate, estimate));
    puts("policy cases passed");
    return 0;
}
