#include "music_library.h"
#include "music_catalog.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "sdkconfig.h"
#if CONFIG_MUSIC_ONLINE_P0
#include "cJSON.h"
#include "music_catalog_protocol.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "music_http_source.h"
#include "music_online_profile.h"
#include "music_network.h"
#include "mbedtls/sha256.h"
#endif

static const char *TAG = "music_library";

#define CACHE_MAGIC 0x4b4d4301u
/* Metadata requests tolerate a slow first TLS handshake, but stay bounded. */
#define CATALOG_CONNECT_ATTEMPTS 3
#define CATALOG_CONNECT_TIMEOUT_MS 5000
#define CATALOG_READY_WAIT_US (20LL * 1000000LL)
#define CATALOG_TOTAL_US (25LL * 1000000LL)
#define JSON_LIMIT 16384
#define CACHE_PATH_BYTES 64

typedef struct {
    uint32_t magic, revision, count;
    music_item_t items[MUSIC_CACHE_MAX];
    uint32_t crc;
} cache_index_t;

typedef enum { REQUEST_PAGE, REQUEST_TRACK, REQUEST_APPLY, REQUEST_LOCAL } request_kind_t;
typedef struct {
    request_kind_t kind;
    uint32_t generation, epoch;
    int index, count;
    bool refresh;
    char release[MUSIC_RELEASE_BYTES];
    music_item_t items[MUSIC_CACHE_MAX];
} request_t;

static QueueHandle_t s_requests;
static music_library_callback_t s_callback;
static atomic_uint s_epoch;
static bool s_mounted;
/* A valid index makes its subset authoritative, including an empty selection.
 * Unindexed legacy leftovers after interrupted deletion must not reappear. */
static bool s_cache_authoritative;
static cache_index_t s_cache;

static void cache_path(const char *hash, char *path, size_t size)
{
    /* SPIFFS names are limited to 32 bytes; the index keeps the full hash. */
    snprintf(path, size, "/music/c_%.24s.op", hash);
}

static void copy_local(music_item_t *item, const char *path)
{
    const size_t length = strlen(path);
    if (length >= sizeof(item->local)) { item->local[0] = 0; return; }
    memcpy(item->local, path, length + 1);
}

static bool legacy_path(const char *path)
{
    for (unsigned i = 0; i < MUSIC_CATALOG_COUNT; ++i) {
        char expected[MUSIC_LOCAL_PATH_BYTES];
        snprintf(expected, sizeof(expected), "/music/%s", MUSIC_CATALOG[i].path);
        if (!strcmp(expected, path)) return true;
    }
    return false;
}

static bool cached_path_valid(const music_item_t *item)
{
    if (strlen(item->sha256) != 64 || item->size == 0 || !item->id[0]) return false;
    for (int i = 0; i < 64; ++i)
        if (!((item->sha256[i] >= '0' && item->sha256[i] <= '9') ||
              (item->sha256[i] >= 'a' && item->sha256[i] <= 'f'))) return false;
    char expected[CACHE_PATH_BYTES];
    cache_path(item->sha256, expected, sizeof(expected));
    return !strcmp(expected, item->local) || legacy_path(item->local);
}

static bool read_index(const char *path, cache_index_t *index)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(index, 1, sizeof(*index), file) == sizeof(*index) && fgetc(file) == EOF;
    fclose(file);
    if (!ok || index->magic != CACHE_MAGIC || index->count > MUSIC_CACHE_MAX ||
        index->crc != esp_rom_crc32_le(0, (const uint8_t *)index, offsetof(cache_index_t, crc)))
        return false;
    for (unsigned i = 0; i < index->count; ++i) {
        const music_item_t *item = &index->items[i];
        if (!memchr(item->id, 0, sizeof(item->id)) ||
            !memchr(item->local, 0, sizeof(item->local)) ||
            !memchr(item->title, 0, sizeof(item->title)) ||
            !memchr(item->sha256, 0, sizeof(item->sha256)) || !cached_path_valid(item)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(index->items[j].id, item->id)) return false;
    }
    return true;
}

static void load_index(void)
{
    s_cache_authoritative = false;
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.magic = CACHE_MAGIC;
    if (!s_mounted) return;
    cache_index_t *a = calloc(2, sizeof(*a));
    if (!a) return;
    bool valid_a = read_index("/music/cache.a", a);
    bool valid_b = read_index("/music/cache.b", a + 1);
    if (valid_a || valid_b) {
        s_cache = valid_b && (!valid_a || a[1].revision > a[0].revision) ? a[1] : a[0];
        s_cache_authoritative = true;
    }
    free(a);
}

static int local_items(music_item_t *items, int capacity)
{
    int count = 0;
    if (!s_mounted) return 0;
    for (unsigned i = 0; !s_cache_authoritative && i < MUSIC_CATALOG_COUNT && count < capacity; ++i) {
        char path[193];
        snprintf(path, sizeof(path), "/music/%s", MUSIC_CATALOG[i].path);
        bool indexed = false;
        for (unsigned j = 0; j < s_cache.count; ++j)
            if (!strcmp(path, s_cache.items[j].local)) indexed = true;
        if (indexed) continue;
        struct stat info;
        if (stat(path, &info) != 0 || info.st_size <= 0) continue;
        music_item_t *item = &items[count];
        memset(item, 0, sizeof(*item));
        snprintf(item->id, sizeof(item->id), "legacy-%u", i);
        snprintf(item->title, sizeof(item->title), "%s", MUSIC_CATALOG[i].title);
        copy_local(item, path);
        item->size = info.st_size;
        item->index = count++;
    }
    for (unsigned i = 0; i < s_cache.count && count < capacity; ++i) {
        struct stat info;
        if (stat(s_cache.items[i].local, &info) != 0 || (uint64_t)info.st_size != s_cache.items[i].size) continue;
        items[count] = s_cache.items[i];
        items[count].online = false;
        items[count].index = count;
        ++count;
    }
    return count;
}

int music_library_boot_local(music_item_t *items, int capacity, bool mounted)
{
    s_mounted = mounted;
    load_index();
    return local_items(items, capacity);
}

static music_library_event_t *new_event(music_library_event_kind_t kind, const request_t *request)
{
    size_t slots = kind == MUSIC_LIBRARY_PAGE ? MUSIC_PAGE_SIZE : kind == MUSIC_LIBRARY_LOCAL ? MUSIC_LOCAL_MAX : kind == MUSIC_LIBRARY_TRACK ? 1 : 0;
    music_library_event_t *event = calloc(1, sizeof(*event) + slots * sizeof(music_item_t));
    if (event) { event->kind = kind; event->generation = request->generation; }
    return event;
}

static bool cancelled(const request_t *request)
{
    return atomic_load(&s_epoch) != request->epoch;
}

static void emit_local(const request_t *request)
{
    music_library_event_t *event = new_event(MUSIC_LIBRARY_LOCAL, request);
    if (!event) return;
    event->count = local_items(event->items, MUSIC_LOCAL_MAX);
    s_callback(event);
}

#if CONFIG_MUSIC_ONLINE_P0
static music_item_t *s_page;
static char s_release[MUSIC_RELEASE_BYTES];
static int s_page_number = -1, s_total;
static int64_t s_manifest_loaded;
/* Worker-owned batch session only: never keep TLS allocated while playing or
 * idle in the UI. Callback userdata must outlive all three HTTP requests. */
static esp_http_client_handle_t s_catalog_client;
static music_http_headers_t s_catalog_headers;
static bool s_catalog_gate;
static unsigned s_catalog_connections;

static void close_catalog_session(void)
{
    if (s_catalog_client) esp_http_client_cleanup(s_catalog_client);
    s_catalog_client = NULL;
    if (s_catalog_gate) music_http_service_release();
    s_catalog_gate = false;
}

static esp_err_t json_headers(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) ++s_catalog_connections;
    if (event->event_id == HTTP_EVENT_ON_HEADER)
        music_http_headers_add(event->user_data, event->header_key, event->header_value);
    return ESP_OK;
}

static esp_err_t get_json(const request_t *request, const char *path, cJSON **result)
{
    *result = NULL;
    const int64_t started = esp_timer_get_time();
    int64_t headers_at = 0;
    if (cancelled(request)) return ESP_ERR_TIMEOUT;
    /* Entering the online list right after boot must not fail only because the
     * clock has not synced yet: HTTPS certificate checks need a valid time. */
    const int64_t ready_deadline = esp_timer_get_time() + CATALOG_READY_WAIT_US;
    while (!music_network_https_ready() && !cancelled(request) &&
           esp_timer_get_time() < ready_deadline)
        vTaskDelay(pdMS_TO_TICKS(200));
    if (!music_network_https_ready()) {
        ESP_LOGW(TAG, "Catalog request %s skipped: network not ready (state %d)", path, music_network_state());
        return ESP_ERR_INVALID_STATE;
    }
    music_online_profile_t profile;
    esp_err_t error = music_online_profile_load(&profile);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Catalog request %s skipped: profile unavailable: %s", path, esp_err_to_name(error));
        return error;
    }
    char url[MUSIC_HTTP_URL_MAX + 1];
    if (!music_http_build_url(profile.origin, path, url, sizeof(url))) {
        music_online_profile_clear(&profile);
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t deadline = esp_timer_get_time() + CATALOG_TOTAL_US;
    while (!s_catalog_gate && !cancelled(request) && esp_timer_get_time() < deadline)
        s_catalog_gate = music_http_service_acquire(50);
    if (!s_catalog_gate) { music_online_profile_clear(&profile); return ESP_ERR_TIMEOUT; }
    const esp_http_client_config_t config = {
        .url = url, .username = profile.username, .password = profile.password,
        .auth_type = HTTP_AUTH_TYPE_BASIC, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .max_authorization_retries = -1,
        .timeout_ms = CATALOG_CONNECT_TIMEOUT_MS, .buffer_size = 512, .buffer_size_tx = 1024,
        .event_handler = json_headers, .user_data = &s_catalog_headers,
    };
    music_http_headers_t *headers = &s_catalog_headers;
    esp_http_client_handle_t client = s_catalog_client;
    /* A fully consumed response permits reuse on the same origin. The SDK
     * reconnects if set_url changes host/port; no redirect is ever followed. */
    if (client && esp_http_client_set_url(client, url) != ESP_OK) {
        close_catalog_session(); music_online_profile_clear(&profile);
        return ESP_ERR_INVALID_ARG;
    }
    char *body = NULL;
    for (int attempt = 0; attempt < CATALOG_CONNECT_ATTEMPTS && !cancelled(request); ++attempt) {
        music_http_headers_init(headers);
        if (!client) client = esp_http_client_init(&config);
        s_catalog_client = client;
        if (!client) { error = ESP_ERR_NO_MEM; break; }
        if (esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK ||
            esp_http_client_set_header(client, "Cache-Control", "no-transform") != ESP_OK) {
            esp_http_client_cleanup(client); client = NULL; error = ESP_FAIL; break;
        }
        const esp_err_t opened = esp_http_client_open(client, 0);
        const int header_bytes = opened == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
        if (opened == ESP_OK && header_bytes >= 0) { headers_at = esp_timer_get_time(); break; }
        int tls_flags = 0, verify = 0;
        (void)esp_http_client_get_and_clear_last_tls_error(client, &verify, &tls_flags);
        ESP_LOGW(TAG, "Catalog %s connect attempt %d failed: open=%s headers=%d tls=0x%x verify=0x%x",
                 path, attempt + 1, esp_err_to_name(opened), header_bytes, tls_flags, verify);
        esp_http_client_cleanup(client); client = NULL;
        error = esp_timer_get_time() >= deadline ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_CONNECT;
        if (error == ESP_ERR_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_catalog_client = client;
    if (!client || cancelled(request)) { if (cancelled(request)) error = ESP_ERR_TIMEOUT; goto done; }
    const int status = esp_http_client_get_status_code(client);
    if (status == 401 || status == 403) {
        error = ESP_ERR_NOT_ALLOWED;
        goto done;
    }
    error = ESP_ERR_INVALID_RESPONSE;
    if (status != 200 || headers->invalid || headers->range_seen || !headers->length_seen ||
        headers->length == 0 || headers->length > JSON_LIMIT || esp_http_client_is_chunked_response(client)) {
        ESP_LOGW(TAG, "Catalog response rejected: HTTP %d, invalid_headers=%d length_seen=%d length=%llu",
                 status, headers->invalid, headers->length_seen, (unsigned long long)headers->length);
        goto done;
    }
    body = malloc((size_t)headers->length + 1);
    if (!body) { error = ESP_ERR_NO_MEM; goto done; }
    size_t used = 0;
    while (used < headers->length) {
        if (cancelled(request) || esp_timer_get_time() >= deadline) { error = ESP_ERR_TIMEOUT; goto done; }
        int got = esp_http_client_read(client, body + used, (int)(headers->length - used));
        if (got <= 0) { error = ESP_FAIL; goto done; }
        used += got;
    }
    if (!esp_http_client_is_complete_data_received(client) || memchr(body, 0, used)) goto done;
    body[used] = 0;
    /* Limit nesting before cJSON recursion; honor quoted/escaped braces. */
    int depth = 0; bool quote = false, escape = false;
    for (size_t i = 0; i < used; ++i) {
        char c = body[i];
        if (quote) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quote = false; }
        else if (c == '"') quote = true;
        else if (c == '[' || c == '{') { if (++depth > 8) goto done; }
        else if (c == ']' || c == '}') { if (--depth < 0) goto done; }
    }
    if (depth || quote || strstr(body, "\\u0000")) goto done;
    *result = cJSON_ParseWithOpts(body, NULL, true);
    error = *result && cJSON_IsObject(*result) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
done:
    free(body);
    /* Keep a successful, fully consumed connection for the next document.
     * Error/cancel paths must never leave an unread response for reuse. */
    if (error != ESP_OK) close_catalog_session();
    music_online_profile_clear(&profile);
    ESP_LOGI(TAG, "Catalog HTTP: %s headers_ms=%lld total_ms=%lld connections=%u",
             path, (long long)(headers_at ? (headers_at - started) / 1000 : -1),
             (long long)((esp_timer_get_time() - started) / 1000), s_catalog_connections);
    if (error != ESP_OK && *result) { cJSON_Delete(*result); *result = NULL; }
    if (error != ESP_OK) ESP_LOGW(TAG, "Catalog request %s failed: %s", path, esp_err_to_name(error));
    return error;
}

static esp_err_t load_manifest(const request_t *request)
{
    cJSON *json = NULL;
    char release[MUSIC_RELEASE_BYTES], manifest[193];
    esp_err_t error = get_json(request, "/v1/current.json", &json);
    if (error != ESP_OK) return error;
    bool valid = music_catalog_current(json, release, manifest);
    cJSON_Delete(json);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    error = get_json(request, manifest, &json);
    if (error != ESP_OK) return error;
    int total;
    valid = music_catalog_manifest(json, release, &total);
    cJSON_Delete(json);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    strcpy(s_release, release);
    s_total = total;
    s_manifest_loaded = esp_timer_get_time();
    s_page_number = -1;
    return ESP_OK;
}

static esp_err_t load_page(const request_t *request, int page)
{
    int count = s_total - page * MUSIC_PAGE_SIZE;
    if (count <= 0) return s_total == 0 && page == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (count > MUSIC_PAGE_SIZE) count = MUSIC_PAGE_SIZE;
    char path[193];
    snprintf(path, sizeof(path), "/v1/releases/%s/tracks/all/%04d.json", s_release, page);
    cJSON *json = NULL;
    esp_err_t error = get_json(request, path, &json);
    /* Page is last: free TLS before allocating item arrays or emitting events. */
    close_catalog_session();
    if (error != ESP_OK) return error;
    s_page_number = -1;
    free(s_page);
    s_page = calloc(MUSIC_PAGE_SIZE, sizeof(*s_page));
    if (!s_page) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
    bool valid = music_catalog_page(json, s_release, page, s_total, s_page);
    cJSON_Delete(json);
    if (!valid) { free(s_page); s_page = NULL; return ESP_ERR_INVALID_RESPONSE; }
    s_page_number = page;
    return ESP_OK;
}

static esp_err_t save_index(cache_index_t *index)
{
    index->magic = CACHE_MAGIC;
    index->revision = s_cache.revision + 1;
    index->crc = esp_rom_crc32_le(0, (const uint8_t *)index, offsetof(cache_index_t, crc));
    const char *destination = index->revision % 2 ? "/music/cache.a" : "/music/cache.b";
    FILE *file = fopen(destination, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(index, 1, sizeof(*index), file) == sizeof(*index);
    if (fclose(file) != 0) ok = false;
    if (!ok) return ESP_FAIL;
    /* Opposite valid slot remains untouched until this one is verified. */
    cache_index_t *verify = malloc(sizeof(*verify));
    if (!verify) return ESP_ERR_NO_MEM;
    ok = read_index(destination, verify) && !memcmp(verify, index, sizeof(*index));
    free(verify);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool verify_cached_file(const request_t *request, const char *path, const music_item_t *item)
{
    struct stat info;
    if (stat(path, &info) != 0 || (uint64_t)info.st_size != item->size) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
    uint8_t chunk[512], digest[32]; size_t got;
    while (ok && (got = fread(chunk, 1, sizeof(chunk), file)) != 0)
        ok = !cancelled(request) && mbedtls_sha256_update(&hash, chunk, got) == 0;
    ok = ok && !ferror(file) && mbedtls_sha256_finish(&hash, digest) == 0;
    fclose(file); mbedtls_sha256_free(&hash);
    if (!ok) return false;
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return !strcmp(hex, item->sha256);
}

static esp_err_t download(const request_t *request, music_item_t *item)
{
    char destination[CACHE_PATH_BYTES], temporary[CACHE_PATH_BYTES];
    cache_path(item->sha256, destination, sizeof(destination));
    snprintf(temporary, sizeof(temporary), "/music/t_%.24s.op", item->sha256);
    /* Recover a fully written orphan from a power loss, but only after hashing.
     * Never replace a same-prefix/different-content file silently. */
    struct stat existing;
    if (stat(destination, &existing) == 0) {
        if (!verify_cached_file(request, destination, item)) return ESP_ERR_INVALID_CRC;
        copy_local(item, destination);
        item->online = false; return ESP_OK;
    }
    size_t total, used;
    if (!s_mounted || esp_spiffs_info("musicfs", &total, &used) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!music_cache_storage_fits(total, used, music_cache_storage_cost(item->size)))
        return MUSIC_ERR_STORAGE_FULL;
    music_online_profile_t profile;
    esp_err_t error = music_online_profile_load(&profile);
    if (error != ESP_OK) return error;
    char remote[193];
    music_item_remote_path(item, remote, sizeof(remote));
    music_source_t source = {0};
    const music_http_source_config_t config = {
        .origin = profile.origin, .audio_path = remote,
        .username = profile.username, .password = profile.password,
        .expected_size = item->size, .buffer_bytes = 4096, .start_bytes = 256,
    };
    error = music_http_source_open(&source, &config);
    music_online_profile_clear(&profile);
    if (error != ESP_OK) return error;
    FILE *file = fopen(temporary, "wb");
    if (!file) { error = errno == ENOSPC ? MUSIC_ERR_STORAGE_FULL : ESP_FAIL; goto close_source; }
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) { fclose(file); mbedtls_sha256_free(&hash); error = ESP_FAIL; goto close_source; }
    uint8_t chunk[512], digest[32];
    uint64_t written = 0;
    int last_progress = -1;
    const int64_t deadline = esp_timer_get_time() + 600000000;
    error = ESP_OK;
    for (;;) {
        if (cancelled(request) || esp_timer_get_time() > deadline) { error = ESP_ERR_TIMEOUT; break; }
        size_t got;
        music_source_result_t result = music_source_read(&source, chunk, sizeof(chunk), &got);
        if (result == MUSIC_SOURCE_AGAIN) { music_http_source_wait(&source); continue; }
        if (result == MUSIC_SOURCE_EOF) break;
        if (result != MUSIC_SOURCE_OK) { error = ESP_FAIL; break; }
        if (fwrite(chunk, 1, got, file) != got) {
            error = errno == ENOSPC ? MUSIC_ERR_STORAGE_FULL : ESP_FAIL; break;
        }
        if (mbedtls_sha256_update(&hash, chunk, got) != 0) { error = ESP_FAIL; break; }
        written += got;
        if (written > item->size) { error = ESP_ERR_INVALID_SIZE; break; }
        int progress = (int)(written * 100 / item->size);
        if (progress >= last_progress + 5) {
            music_library_event_t *event = new_event(MUSIC_LIBRARY_PROGRESS, request);
            if (event) { event->progress = progress; s_callback(event); }
            last_progress = progress;
        }
    }
    if (fclose(file) != 0) error = ESP_FAIL;
    if (mbedtls_sha256_finish(&hash, digest) != 0) error = ESP_FAIL;
    mbedtls_sha256_free(&hash);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (error == ESP_OK && (written != item->size || strcmp(hex, item->sha256))) error = ESP_ERR_INVALID_CRC;
    if (error == ESP_OK && cancelled(request)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK) {
        if (rename(temporary, destination) != 0) error = ESP_FAIL;
        else {
            copy_local(item, destination);
            item->online = false;
        }
    }
    if (error != ESP_OK) remove(temporary); /* Only this request's temporary output, never an indexed song. */
close_source:
    music_source_cancel(&source);
    music_source_close(&source);
    return error;
}

/* Only files owned by this application may be pruned. Unknown files and
 * cache index slots are never included in this allowlist. */
static bool managed_audio(const char *path)
{
    if (legacy_path(path)) return true;
    if (strncmp(path, "/music/", 7)) return false;
    const char *name = path + 7;
    if (strlen(name) != 29 || (name[0] != 'c' && name[0] != 't') ||
        name[1] != '_' || strcmp(name + 26, ".op")) return false;
    for (int i = 2; i < 26; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}

static esp_err_t prune_audio(const cache_index_t *keep)
{
    /* Restart enumeration after each removal: no iterator invalidation assumptions. */
    for (;;) {
        DIR *dir = opendir("/music");
        if (!dir) return ESP_FAIL;
        struct dirent *entry;
        char victim[CACHE_PATH_BYTES] = {0};
        while ((entry = readdir(dir))) {
            char path[CACHE_PATH_BYTES];
            if (snprintf(path, sizeof(path), "/music/%s", entry->d_name) >= (int)sizeof(path)) continue;
            if (!managed_audio(path)) continue;
            bool retained = false;
            for (unsigned i = 0; i < keep->count; ++i)
                if (!strcmp(path, keep->items[i].local)) retained = true;
            if (!retained) { strcpy(victim, path); break; }
        }
        closedir(dir);
        if (!victim[0]) return ESP_OK;
        if (remove(victim) != 0 && errno != ENOENT) return ESP_FAIL;
        ESP_LOGI(TAG, "Cache removed: %s", victim);
    }
}

static esp_err_t apply(const request_t *request)
{
    if (!s_mounted || request->count < 0 || request->count > MUSIC_CACHE_MAX) return ESP_ERR_INVALID_ARG;
    size_t total, used;
    if (esp_spiffs_info("musicfs", &total, &used) != ESP_OK) return ESP_FAIL;
    /* Preflight before any deletion: full final set, page overhead, both index
     * slots and unknown files. Existing managed songs will be replaced. */
    uint64_t needed = 8192;
    for (int i = 0; i < request->count; ++i) {
        music_item_t item = request->items[i];
        char path[CACHE_PATH_BYTES]; cache_path(item.sha256, path, sizeof(path));
        copy_local(&item, path);
        if (!cached_path_valid(&item)) return ESP_ERR_INVALID_ARG;
        for (int j = 0; j < i; ++j)
            if (!strcmp(item.id, request->items[j].id) ||
                !strncmp(item.sha256, request->items[j].sha256, 24)) return ESP_ERR_INVALID_ARG;
        uint64_t cost = music_cache_storage_cost(item.size);
        if (cost > UINT64_MAX - needed) return MUSIC_ERR_STORAGE_FULL;
        needed += cost;
    }
    DIR *dir = opendir("/music");
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char path[CACHE_PATH_BYTES]; struct stat info;
        if (snprintf(path, sizeof(path), "/music/%s", entry->d_name) >= (int)sizeof(path)) {
            closedir(dir); return ESP_FAIL;
        }
        if (managed_audio(path) || !strcmp(path, "/music/cache.a") || !strcmp(path, "/music/cache.b")) continue;
        if (stat(path, &info) != 0 || info.st_size < 0) { closedir(dir); return ESP_FAIL; }
        uint64_t cost = music_cache_storage_cost((uint64_t)info.st_size);
        if (cost > UINT64_MAX - needed) { closedir(dir); return MUSIC_ERR_STORAGE_FULL; }
        needed += cost;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Cache plan: selected=%d estimated=%llu total=%u used=%u",
             request->count, (unsigned long long)needed, (unsigned)total, (unsigned)used);
    if (!music_cache_storage_fits(total, 0, needed)) return MUSIC_ERR_STORAGE_FULL;

    cache_index_t *next = calloc(1, sizeof(*next));
    if (!next) return ESP_ERR_NO_MEM;
    esp_err_t error = ESP_OK;
    /* Reuse only size + SHA verified files, including authorized legacy songs.
     * Keeping their existing path avoids copying the same bytes during migration. */
    for (int i = 0; i < request->count; ++i) {
        music_item_t item = request->items[i]; item.local[0] = 0;
        char candidate[CACHE_PATH_BYTES];
        cache_path(item.sha256, candidate, sizeof(candidate));
        if (verify_cached_file(request, candidate, &item)) copy_local(&item, candidate);
        for (unsigned j = 0; !item.local[0] && j < MUSIC_CATALOG_COUNT; ++j) {
            snprintf(candidate, sizeof(candidate), "/music/%s", MUSIC_CATALOG[j].path);
            if (verify_cached_file(request, candidate, &item)) copy_local(&item, candidate);
        }
        if (item.local[0]) { item.online = false; next->items[next->count++] = item; }
    }
    if (cancelled(request)) { error = ESP_ERR_TIMEOUT; goto done; }
    /* Publish the retained subset BEFORE removing anything. From this point
     * replacement is intentionally partial on cancellation/failure, not rollback. */
    error = save_index(next);
    if (error != ESP_OK) goto done;
    s_cache = *next;
    s_cache_authoritative = true;
    ESP_LOGI(TAG, "Cache retained: count=%u", (unsigned)s_cache.count);
    error = prune_audio(next);
    if (error != ESP_OK) goto done;
    for (int i = 0; i < request->count; ++i) {
        if (cancelled(request)) { error = ESP_ERR_TIMEOUT; break; }
        bool retained = false;
        for (unsigned j = 0; j < next->count; ++j)
            if (!strcmp(next->items[j].id, request->items[i].id)) retained = true;
        if (retained) continue;
        music_item_t *item = &next->items[next->count];
        *item = request->items[i]; item->local[0] = 0;
        error = download(request, item);
        if (error != ESP_OK) break;
        ++next->count;
        error = save_index(next);
        if (error != ESP_OK) break;
        s_cache = *next;
        ESP_LOGI(TAG, "Cache committed: count=%u", (unsigned)s_cache.count);
    }
done:
    ESP_LOGI(TAG, "Cache apply ended: count=%u result=%s", (unsigned)s_cache.count, esp_err_to_name(error));
    free(next);
    return error;
}
#endif

static void emit_error(const request_t *request, esp_err_t error)
{
    music_library_event_t *event = new_event(MUSIC_LIBRARY_ERROR, request);
    if (event) { event->error = error; s_callback(event); }
    else ESP_LOGW(TAG, "cannot report %s: out of memory", esp_err_to_name(error));
}

static void worker(void *argument)
{
    (void)argument;
    request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE || cancelled(&request)) continue;
        ESP_LOGI(TAG, "request kind=%d index=%d refresh=%d count=%d", (int)request.kind, request.index, request.refresh, request.count);
        if (request.kind == REQUEST_LOCAL) { emit_local(&request); continue; }
        esp_err_t error = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_MUSIC_ONLINE_P0
        const int64_t request_started = esp_timer_get_time();
        s_catalog_connections = 0;
        if (request.kind == REQUEST_APPLY) {
            error = apply(&request);
            if (error == ESP_OK && !cancelled(&request)) {
                music_library_event_t *event = new_event(MUSIC_LIBRARY_DONE, &request);
                if (event) s_callback(event); else error = ESP_ERR_NO_MEM;
            }
        } else {
            error = ESP_OK;
            /* Track requests remain pinned to the displayed release. Pages
             * revalidate after five minutes; explicit Refresh always revalidates. */
            if (request.refresh || !s_release[0] ||
                (request.kind == REQUEST_PAGE && esp_timer_get_time() - s_manifest_loaded > 300000000LL))
                error = load_manifest(&request);
            if (request.kind == REQUEST_TRACK && error == ESP_OK && strcmp(request.release, s_release)) error = ESP_ERR_INVALID_STATE;
            int page = request.kind == REQUEST_TRACK ? request.index / MUSIC_PAGE_SIZE : request.index;
            if (error == ESP_OK) error = load_page(&request, page);
            close_catalog_session();
            ESP_LOGI(TAG, "Catalog ready: result=%s page=%d total=%d elapsed_ms=%lld connections=%u",
                     esp_err_to_name(error), page, s_total,
                     (long long)((esp_timer_get_time() - request_started) / 1000), s_catalog_connections);
            if (error == ESP_OK && !cancelled(&request)) {
                music_library_event_t *event = new_event(request.kind == REQUEST_TRACK ? MUSIC_LIBRARY_TRACK : MUSIC_LIBRARY_PAGE, &request);
                if (event) {
                    event->total = s_total; event->page = page; strcpy(event->release, s_release);
                    event->count = s_total - page * MUSIC_PAGE_SIZE;
                    if (event->count > MUSIC_PAGE_SIZE) event->count = MUSIC_PAGE_SIZE;
                    if (request.kind == REQUEST_TRACK) {
                        if (request.index < 0 || request.index >= s_total) { free(event); error = ESP_ERR_INVALID_ARG; }
                        else {
                            event->items[0] = s_page[request.index % MUSIC_PAGE_SIZE]; event->count = 1;
                            free(s_page); s_page = NULL; s_page_number = -1;
                            s_callback(event);
                        }
                    } else {
                        if (event->count) memcpy(event->items, s_page, (size_t)event->count * sizeof(*s_page));
                        free(s_page); s_page = NULL; s_page_number = -1;
                        s_callback(event);
                    }
                } else {
                    /* A missing event must still end the UI's loading state. */
                    error = ESP_ERR_NO_MEM;
                }
            }
        }
#endif
#if CONFIG_MUSIC_ONLINE_P0
        close_catalog_session();
        free(s_page); s_page = NULL; s_page_number = -1;
#endif
        if (error != ESP_OK && !cancelled(&request)) {
            ESP_LOGW(TAG, "posting error event %s", esp_err_to_name(error));
            emit_error(&request, error);
        }
    }
}

esp_err_t music_library_start(music_library_callback_t callback, bool mounted)
{
    if (s_requests || !callback) return ESP_ERR_INVALID_STATE;
    s_callback = callback; s_mounted = mounted;
    s_requests = xQueueCreate(1, sizeof(request_t));
    if (!s_requests) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "music_library", 8192, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_requests); s_requests = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void music_library_cancel(void) { atomic_fetch_add(&s_epoch, 1); }

static bool submit(request_t *request)
{
    if (!s_requests) return false;
    request->epoch = atomic_fetch_add(&s_epoch, 1) + 1;
    return xQueueOverwrite(s_requests, request) == pdTRUE;
}

bool music_library_page(int page, bool refresh, uint32_t generation)
{
    if (page < 0 || page >= MUSIC_REMOTE_MAX / MUSIC_PAGE_SIZE) return false;
    request_t request = {.kind = REQUEST_PAGE, .generation = generation, .index = page, .refresh = refresh};
    return submit(&request);
}

bool music_library_track(int index, const char *release, uint32_t generation)
{
    if (!release || strlen(release) >= MUSIC_RELEASE_BYTES || index < 0 || index >= MUSIC_REMOTE_MAX) return false;
    request_t request = {.kind = REQUEST_TRACK, .generation = generation, .index = index};
    strcpy(request.release, release);
    return submit(&request);
}

bool music_library_apply(const music_item_t *items, int count, uint32_t generation)
{
    if (count < 0 || count > MUSIC_CACHE_MAX || (!items && count)) return false;
    request_t request = {.kind = REQUEST_APPLY, .generation = generation, .count = count};
    if (count) memcpy(request.items, items, (size_t)count * sizeof(*items));
    return submit(&request);
}

bool music_library_local(uint32_t generation)
{
    request_t request = {.kind = REQUEST_LOCAL, .generation = generation};
    return submit(&request);
}
