#include "music_catalog_protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *manifest_text =
    "{\"schema_version\":1,\"release_id\":\"r1\",\"total_tracks\":21,\"page_size\":20,\"total_pages\":2,"
    "\"audio_format\":{\"codec\":\"opus\",\"container\":\"length-prefixed-le16\",\"sample_rate\":16000,\"channels\":1,\"bitrate\":32000,\"frame_ms\":20}}";
static void set(cJSON *json, const char *key, cJSON *value) { cJSON_ReplaceItemInObjectCaseSensitive(json, key, value); }

static void current(void)
{
    char release[MUSIC_RELEASE_BYTES], path[193];
    cJSON *json = cJSON_Parse("{\"schema_version\":1,\"release_id\":\"r1\",\"manifest_path\":\"/v1/releases/r1/manifest.json\"}");
    assert(music_catalog_current(json, release, path));
    assert(!strcmp(release, "r1"));
    set(json, "manifest_path", cJSON_CreateString("https://evil.test/a"));
    assert(!music_catalog_current(json, release, path));
    set(json, "manifest_path", cJSON_CreateString("/v1/releases/r1/manifest.json"));
    set(json, "release_id", cJSON_CreateString("../r1"));
    assert(!music_catalog_current(json, release, path));
    set(json, "release_id", cJSON_CreateString("r1"));
    cJSON_AddNumberToObject(json, "schema_version", 1);
    assert(!music_catalog_current(json, release, path));
    cJSON_Delete(json);
    assert(!music_catalog_current(NULL, release, path));
}

static void manifest(void)
{
    int total;
    cJSON *json = cJSON_Parse(manifest_text);
    assert(music_catalog_manifest(json, "r1", &total) && total == 21);
    assert(!music_catalog_manifest(json, "r2", &total));
    set(json, "total_tracks", cJSON_CreateNumber(1001));
    assert(!music_catalog_manifest(json, "r1", &total));
    set(json, "total_tracks", cJSON_CreateNumber(21.5));
    assert(!music_catalog_manifest(json, "r1", &total));
    set(json, "total_tracks", cJSON_CreateNumber(0));
    set(json, "total_pages", cJSON_CreateNumber(0));
    assert(music_catalog_manifest(json, "r1", &total) && total == 0);
    set(json, "total_tracks", cJSON_CreateNumber(1000));
    set(json, "total_pages", cJSON_CreateNumber(50));
    assert(music_catalog_manifest(json, "r1", &total) && total == 1000);
    set(json, "page_size", cJSON_CreateNumber(30));
    assert(!music_catalog_manifest(json, "r1", &total));
    set(json, "page_size", cJSON_CreateNumber(20));
    cJSON *format = cJSON_GetObjectItemCaseSensitive(json, "audio_format");
    set(format, "sample_rate", cJSON_CreateNumber(48000));
    assert(!music_catalog_manifest(json, "r1", &total));
    set(format, "sample_rate", cJSON_CreateNumber(16000));
    cJSON_AddItemToObject(json, "audio_format", cJSON_Duplicate(format, true));
    assert(!music_catalog_manifest(json, "r1", &total));
    cJSON_Delete(json);
}

static cJSON *track(void)
{
    cJSON *json = cJSON_CreateObject();
    char hash[65], path[193]; memset(hash, 'a', 64); hash[64] = 0;
    snprintf(path, sizeof(path), "/v1/audio/%s.opus", hash);
    cJSON_AddStringToObject(json, "id", "song-1");
    cJSON_AddStringToObject(json, "title", "小星星");
    cJSON_AddStringToObject(json, "sha256", hash);
    cJSON_AddStringToObject(json, "audio_path", path);
    cJSON_AddNumberToObject(json, "size_bytes", 100);
    return json;
}

static void page(void)
{
    music_item_t *items = calloc(MUSIC_PAGE_SIZE, sizeof(*items));
    assert(items);
    cJSON *json = cJSON_CreateObject(), *tracks = cJSON_CreateArray(), *one = track();
    cJSON_AddNumberToObject(json, "schema_version", 1);
    cJSON_AddStringToObject(json, "release_id", "r1");
    cJSON_AddNumberToObject(json, "page", 1);
    cJSON_AddNumberToObject(json, "total_pages", 2);
    cJSON_AddItemToArray(tracks, one);
    cJSON_AddItemToObject(json, "tracks", tracks);
    assert(music_catalog_page(json, "r1", 1, 21, items));
    assert(items[0].index == 20 && items[0].size == 100 && items[0].online);
    assert(!music_catalog_page(json, "r2", 1, 21, items));
    assert(!music_catalog_page(json, "r1", 0, 21, items));
    assert(!music_catalog_page(json, "r1", 2, 21, items));
    set(one, "size_bytes", cJSON_CreateNumber(-1));
    assert(!music_catalog_page(json, "r1", 1, 21, items));
    set(one, "size_bytes", cJSON_CreateNumber(100.5));
    assert(!music_catalog_page(json, "r1", 1, 21, items));
    set(one, "size_bytes", cJSON_CreateNumber(100));
    set(one, "title", cJSON_CreateString("\xc0\xaf"));
    assert(!music_catalog_page(json, "r1", 1, 21, items));
    set(one, "title", cJSON_CreateString("song\nname"));
    assert(!music_catalog_page(json, "r1", 1, 21, items));
    set(one, "title", cJSON_CreateString("小星星"));
    set(one, "audio_path", cJSON_CreateString("/../../audio/test.opus"));
    assert(!music_catalog_page(json, "r1", 1, 21, items));
    cJSON_DeleteItemFromArray(tracks, 0); cJSON_AddItemToArray(tracks, track());
    cJSON_AddItemToArray(tracks, track());
    assert(!music_catalog_page(json, "r1", 1, 22, items)); /* duplicate ID */
    cJSON_DeleteItemFromArray(tracks, 1);
    cJSON_AddItemToObject(json, "tracks", cJSON_CreateArray());
    assert(!music_catalog_page(json, "r1", 1, 21, items)); /* duplicate container */
    cJSON_Delete(json); free(items);
}

static cJSON *read_document(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); assert(length > 0 && length <= 16384);
    rewind(file);
    char *text = calloc((size_t)length + 1, 1); assert(text);
    assert(fread(text, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    cJSON *json = cJSON_ParseWithOpts(text, NULL, true);
    free(text); assert(json && cJSON_IsObject(json)); return json;
}

static void documents(const char *current_path, const char *manifest_path, const char *page_path)
{
    char release[MUSIC_RELEASE_BYTES], path[193]; int total;
    cJSON *current_json = read_document(current_path);
    assert(music_catalog_current(current_json, release, path)); cJSON_Delete(current_json);
    cJSON *manifest_json = read_document(manifest_path);
    assert(music_catalog_manifest(manifest_json, release, &total)); cJSON_Delete(manifest_json);
    if (total) {
        music_item_t *items = calloc(MUSIC_PAGE_SIZE, sizeof(*items)); assert(items);
        cJSON *page_json = read_document(page_path);
        assert(music_catalog_page(page_json, release, 0, total, items));
        cJSON_Delete(page_json); free(items);
    }
    puts("current, manifest and page accepted by production validator");
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "documents")) {
        documents(argv[2], argv[3], argv[4]); return 0;
    }
    assert(argc == 2);
    if (!strcmp(argv[1], "current")) current();
    else if (!strcmp(argv[1], "manifest")) manifest();
    else if (!strcmp(argv[1], "page")) page();
    else return 2;
    return 0;
}
