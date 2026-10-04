#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "music_policy.h"

/* Distinguish persistent Flash capacity from transient RAM allocation. */
#define MUSIC_ERR_STORAGE_FULL ((esp_err_t)0x7601)

typedef enum {
    MUSIC_LIBRARY_PAGE, MUSIC_LIBRARY_LOCAL, MUSIC_LIBRARY_TRACK,
    MUSIC_LIBRARY_PROGRESS, MUSIC_LIBRARY_ERROR, MUSIC_LIBRARY_DONE,
} music_library_event_kind_t;

typedef struct {
    music_library_event_kind_t kind;
    uint32_t generation;
    esp_err_t error;
    int count, total, page, progress;
    char release[MUSIC_RELEASE_BYTES];
    music_item_t items[]; /* Event-owned bounded payload: one track or at most one page. */
} music_library_event_t;

/* Callback takes ownership even on failure; it must enqueue/free, never use LVGL. */
typedef void (*music_library_callback_t)(music_library_event_t *event);

esp_err_t music_library_start(music_library_callback_t callback, bool mounted);
void music_library_cancel(void);
bool music_library_page(int page, bool refresh, uint32_t generation);
bool music_library_track(int index, const char *release, uint32_t generation);
bool music_library_apply(const music_item_t *items, int count, uint32_t generation);
bool music_library_local(uint32_t generation);
/* Used only during bootstrap, before UI/tasks start; filesystem work stays off UI. */
int music_library_boot_local(music_item_t *items, int capacity, bool mounted);

/* Pure policy helper, shared by firmware and host tests. */
bool music_cache_toggle(music_item_t *selected, int *count, const music_item_t *item);
int music_playlist_step(const uint16_t *order, int count, int position, int direction);
