#include "music_catalog_protocol.h"
#include <stdio.h>
#include <string.h>

static const cJSON *unique(const cJSON *object, const char *key)
{
    if (!cJSON_IsObject(object)) return NULL;
    const cJSON *result = NULL;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (item->string && !strcmp(item->string, key)) {
            if (result) return NULL;
            result = item;
        }
    }
    return result;
}

static bool valid_text(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t code, minimum; int remain;
        if (*p < 128) { if (*p < 32 || *p == 127) return false; ++p; continue; }
        if ((*p & 0xe0) == 0xc0) { code = *p++ & 31; remain = 1; minimum = 128; }
        else if ((*p & 0xf0) == 0xe0) { code = *p++ & 15; remain = 2; minimum = 2048; }
        else if ((*p & 0xf8) == 0xf0) { code = *p++ & 7; remain = 3; minimum = 65536; }
        else return false;
        while (remain--) { if ((*p & 0xc0) != 0x80) return false; code = code << 6 | (*p++ & 63); }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

static bool string(const cJSON *object, const char *key, char *out, size_t size)
{
    const cJSON *value = unique(object, key);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0] ||
        strlen(value->valuestring) >= size || !valid_text(value->valuestring)) return false;
    strcpy(out, value->valuestring); return true;
}

static bool number(const cJSON *object, const char *key, int low, int high, int *out)
{
    const cJSON *value = unique(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < low || value->valuedouble > high ||
        value->valuedouble != (double)value->valueint) return false;
    *out = value->valueint; return true;
}

static bool identifier(const char *text)
{
    if (!*text) return false;
    for (; *text; ++text) if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') ||
        (*text >= '0' && *text <= '9') || *text == '-' || *text == '_')) return false;
    return true;
}

bool music_hash_valid(const char *hash)
{
    if (!hash || strlen(hash) != 64) return false;
    for (int i = 0; i < 64; ++i) if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return false;
    return true;
}

bool music_catalog_current(const cJSON *json, char *release, char *manifest)
{
    int schema; char expected[193];
    if (!number(json, "schema_version", 1, 1, &schema) ||
        !string(json, "release_id", release, MUSIC_RELEASE_BYTES) || !identifier(release) ||
        !string(json, "manifest_path", manifest, 193)) return false;
    snprintf(expected, sizeof(expected), "/v1/releases/%s/manifest.json", release);
    return !strcmp(expected, manifest);
}

bool music_catalog_manifest(const cJSON *json, const char *release, int *total)
{
    int schema, size, pages, rate, channels, bitrate, frame;
    char echoed[MUSIC_RELEASE_BYTES], codec[16], container[48];
    const cJSON *format = unique(json, "audio_format");
    return number(json, "schema_version", 1, 1, &schema) &&
        string(json, "release_id", echoed, sizeof(echoed)) && !strcmp(release, echoed) &&
        number(json, "total_tracks", 0, MUSIC_REMOTE_MAX, total) &&
        number(json, "page_size", MUSIC_PAGE_SIZE, MUSIC_PAGE_SIZE, &size) &&
        number(json, "total_pages", 0, MUSIC_REMOTE_MAX / MUSIC_PAGE_SIZE, &pages) &&
        pages == (*total + MUSIC_PAGE_SIZE - 1) / MUSIC_PAGE_SIZE && cJSON_IsObject(format) &&
        string(format, "codec", codec, sizeof(codec)) && !strcmp(codec, "opus") &&
        string(format, "container", container, sizeof(container)) && !strcmp(container, "length-prefixed-le16") &&
        number(format, "sample_rate", 16000, 16000, &rate) && number(format, "channels", 1, 1, &channels) &&
        number(format, "bitrate", 32000, 32000, &bitrate) && number(format, "frame_ms", 20, 20, &frame);
}

bool music_catalog_page(const cJSON *json, const char *release, int page, int total, music_item_t *items)
{
    if (!items || page < 0 || page >= MUSIC_REMOTE_MAX / MUSIC_PAGE_SIZE ||
        total <= 0 || total > MUSIC_REMOTE_MAX || page * MUSIC_PAGE_SIZE >= total) return false;
    int count = total - page * MUSIC_PAGE_SIZE;
    if (count > MUSIC_PAGE_SIZE) count = MUSIC_PAGE_SIZE;
    int schema, page_value, pages;
    char echoed[MUSIC_RELEASE_BYTES];
    const cJSON *tracks = unique(json, "tracks");
    if (!number(json, "schema_version", 1, 1, &schema) ||
        !string(json, "release_id", echoed, sizeof(echoed)) || strcmp(release, echoed) ||
        !number(json, "page", page, page, &page_value) ||
        !number(json, "total_pages", 1, MUSIC_REMOTE_MAX / MUSIC_PAGE_SIZE, &pages) ||
        pages != (total + MUSIC_PAGE_SIZE - 1) / MUSIC_PAGE_SIZE ||
        !cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) != count) return false;
    memset(items, 0, MUSIC_PAGE_SIZE * sizeof(*items));
    for (int i = 0; i < count; ++i) {
        const cJSON *track = cJSON_GetArrayItem(tracks, i);
        music_item_t *item = &items[i]; int bytes;
        char audio_path[193], expected[193];
        if (!string(track, "id", item->id, sizeof(item->id)) || !identifier(item->id) ||
            !string(track, "title", item->title, sizeof(item->title)) ||
            !string(track, "sha256", item->sha256, sizeof(item->sha256)) || !music_hash_valid(item->sha256) ||
            !string(track, "audio_path", audio_path, sizeof(audio_path)) ||
            !number(track, "size_bytes", 1, 64 * 1024 * 1024, &bytes)) return false;
        item->size = bytes; item->online = true; item->index = page * MUSIC_PAGE_SIZE + i;
        music_item_remote_path(item, expected, sizeof(expected));
        if (strcmp(expected, audio_path)) return false;
        for (int j = 0; j < i; ++j) if (!strcmp(items[j].id, item->id)) return false;
    }
    return true;
}
