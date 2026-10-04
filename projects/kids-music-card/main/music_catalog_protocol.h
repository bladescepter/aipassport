#pragma once
#include "music_policy.h"
#include "cJSON.h"

bool music_catalog_current(const cJSON *json, char *release, char *manifest);
bool music_catalog_manifest(const cJSON *json, const char *release, int *total);
bool music_catalog_page(const cJSON *json, const char *release, int page, int total, music_item_t *items);
bool music_hash_valid(const char *hash);
