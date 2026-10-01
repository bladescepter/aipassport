/* Pure-C URL, header, response and recovery policy tests; not an IDF mock. */
#include "music_http_protocol.h"
#include "music_frame_reader.h"
#include "music_stream_buffer.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) assert(expression)

static music_http_headers_t response(const char *length, const char *range)
{
    music_http_headers_t headers;
    music_http_headers_init(&headers);
    if (length) {
        CHECK(music_http_headers_add(&headers, "Content-Length", length));
    }
    if (range) {
        CHECK(music_http_headers_add(&headers, "Content-Range", range));
    }
    return headers;
}

static void url_allowed(void)
{
    const char *origins[] = {
        "https://music.example.test", "https://127.0.0.1:8443", "https://music-1.example.test:443",
    };
    const char *paths[] = {"/track.opus", "/v1/audio/abc123.opus", "/v1/releases/release-1/manifest.json"};
    char url[MUSIC_HTTP_URL_MAX + 1];
    for (size_t i = 0; i < sizeof(origins) / sizeof(origins[0]); i++) {
        for (size_t j = 0; j < sizeof(paths) / sizeof(paths[0]); j++) {
            CHECK(music_http_build_url(origins[i], paths[j], url, sizeof(url)));
            CHECK(strncmp(url, origins[i], strlen(origins[i])) == 0);
            CHECK(strcmp(url + strlen(origins[i]), paths[j]) == 0);
        }
    }
}

static void url_rejected(void)
{
    const char *origins[] = {
        "http://music.example.test", "HTTPS://music.example.test", "https://",
        "https://user:pass@music.example.test", "https://music.example.test/path",
        "https://music.example.test/", "https://music.example.test?token=private",
        "https://music.example.test#fragment", "https://-host.test", "https://host-.test",
        "https://host..test", "https://.host", "https://host.", "https://host:0",
        "https://host:65536", "https://host:443:80", "https://host:", "https://host:abc",
        "https://host\\evil.test", "https://host\r\nAuthorization: bad", "https://[::1]",
    };
    const char *paths[] = {
        "track.opus", "/", "//evil.test/song", "/a//b", "/a/", "/./track", "/../track",
        "/a/../track", "/a/./track", "/%2e%2e/track", "/%252e/track", "/a\\b",
        "/https://evil.test/track", "/song?token=private", "/song#x", "/song\r\nX: bad",
        "/song name.opus", "/\xe4\xb8\xad.opus",
    };
    char url[MUSIC_HTTP_URL_MAX + 1];
    for (size_t i = 0; i < sizeof(origins) / sizeof(origins[0]); i++) {
        strcpy(url, "old");
        CHECK(!music_http_build_url(origins[i], "/song.opus", url, sizeof(url)));
        CHECK(url[0] == '\0');
    }
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        CHECK(!music_http_build_url("https://host.test", paths[i], url, sizeof(url)));
    }
    CHECK(!music_http_build_url(NULL, "/song", url, sizeof(url)));
    CHECK(!music_http_build_url("https://host", NULL, url, sizeof(url)));
    CHECK(!music_http_build_url("https://host", "/song", NULL, 0));
}

static void url_boundaries(void)
{
    char url[MUSIC_HTTP_URL_MAX + 1];
    const char *origin = "https://host.test", *path = "/song.opus";
    const size_t size = strlen(origin) + strlen(path);
    CHECK(!music_http_build_url(origin, path, url, size));
    CHECK(music_http_build_url(origin, path, url, size + 1));
    char long_path[MUSIC_HTTP_PATH_MAX + 2];
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    long_path[MUSIC_HTTP_PATH_MAX] = '\0';
    CHECK(music_http_build_url(origin, long_path, url, sizeof(url)));
    long_path[MUSIC_HTTP_PATH_MAX] = 'a';
    long_path[MUSIC_HTTP_PATH_MAX + 1] = '\0';
    CHECK(!music_http_build_url(origin, long_path, url, sizeof(url)));
    char label[80] = "https://";
    memset(label + 8, 'a', 64);
    label[72] = '\0';
    CHECK(!music_http_build_url(label, path, url, sizeof(url)));
    label[71] = '\0';
    CHECK(music_http_build_url(label, path, url, sizeof(url)));
}

static void header_numbers(void)
{
    const char *valid[] = {"0", "9", "0009", " \t738000\t ", "18446744073709551615"};
    const uint64_t expected[] = {0, 9, 9, 738000, UINT64_MAX};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        music_http_headers_t headers = response(valid[i], NULL);
        CHECK(headers.length_seen && headers.length == expected[i]);
    }
    const char *invalid[] = {"", " ", "-1", "+9", "1.0", "9x", "9,9", "9 9", "9\n", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        music_http_headers_t headers = {0};
        CHECK(!music_http_headers_add(&headers, "Content-Length", invalid[i]));
        CHECK(headers.invalid);
    }
}

static void header_ranges(void)
{
    music_http_headers_t headers = response("5", " \tbytes 4-8/9\t");
    CHECK(headers.range_start == 4 && headers.range_end == 8 && headers.range_total == 9);
    const char *invalid[] = {
        "", "items 4-8/9", "bytes */9", "bytes 4-8/*", "bytes 8-4/9", "bytes 4-9/9",
        "bytes 0-0/0", "bytes -1-8/9", "bytes 4-8/9x", "bytes 4-8/9, bytes 4-8/9",
        "bytes 18446744073709551616-8/9", "bytes 4-8/18446744073709551616",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        music_http_headers_init(&headers);
        CHECK(!music_http_headers_add(&headers, "Content-Range", invalid[i]));
        CHECK(headers.invalid);
    }
}

static void header_duplicates(void)
{
    music_http_headers_t headers = response("9", NULL);
    CHECK(!music_http_headers_add(&headers, "content-LENGTH", "9"));
    CHECK(headers.invalid);
    CHECK(!music_http_headers_add(&headers, "X-Other", "value"));
    headers = response("5", "bytes 4-8/9");
    CHECK(!music_http_headers_add(&headers, "CONTENT-range", "bytes 4-8/9"));
    music_http_headers_init(&headers);
    CHECK(music_http_headers_add(&headers, "CONTENT-ENCODING", "identity"));
    CHECK(!music_http_headers_add(&headers, "Content-Encoding", "identity"));
}

static void header_encoding(void)
{
    music_http_headers_t headers = response("9", NULL);
    CHECK(music_http_headers_add(&headers, "Content-Encoding", " Identity "));
    CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_ACCEPT);
    const char *invalid[] = {"gzip", "br", "deflate", "identity,gzip", ""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        headers = response("9", NULL);
        CHECK(!music_http_headers_add(&headers, "Content-Encoding", invalid[i]));
        CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_REJECT);
    }
    headers = response("9", NULL);
    CHECK(!music_http_headers_add(&headers, "Transfer-Encoding", "chunked"));
    CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_REJECT);
}

static void header_limits(void)
{
    music_http_headers_t headers = {0};
    CHECK(music_http_headers_add(&headers, "X-Unknown", "ignored"));
    CHECK(!headers.length_seen && !headers.range_seen && !headers.invalid);
    CHECK(!music_http_headers_add(&headers, NULL, "value"));
    music_http_headers_init(&headers);
    CHECK(!music_http_headers_add(&headers, "X", NULL));
    char value[258];
    memset(value, 'a', sizeof(value));
    value[256] = '\0';
    music_http_headers_init(&headers);
    CHECK(music_http_headers_add(&headers, "X", value));
    value[256] = 'a';
    value[257] = '\0';
    CHECK(!music_http_headers_add(&headers, "X", value));
    music_http_headers_init(&headers);
    CHECK(!music_http_headers_add(&headers, "X", "value\r\nInjected: yes"));
    music_http_headers_init(&headers);
    CHECK(!music_http_headers_add(&headers, "X\r\nInjected", "value"));
    music_http_headers_init(&headers);
    CHECK(!music_http_headers_add(&headers, "X", "value\x7f"));
}

static void response_initial(void)
{
    music_http_headers_t headers = response("9", NULL);
    CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_ACCEPT);
    CHECK(music_http_validate_response(200, &headers, 0, 8) == MUSIC_HTTP_REJECT);
    CHECK(music_http_validate_response(200, &headers, 0, 10) == MUSIC_HTTP_REJECT);
    CHECK(music_http_validate_response(200, &headers, 0, 0) == MUSIC_HTTP_REJECT);
    CHECK(music_http_validate_response(200, &headers, 0, UINT64_MAX) == MUSIC_HTTP_REJECT);
    headers = response(NULL, NULL);
    CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_REJECT);
    headers = response("9", "bytes 0-8/9");
    CHECK(music_http_validate_response(206, &headers, 0, 9) == MUSIC_HTTP_ACCEPT);
    CHECK(music_http_validate_response(200, &headers, 0, 9) == MUSIC_HTTP_REJECT);
}

static void response_resume(void)
{
    music_http_headers_t headers = response("5", "bytes 4-8/9");
    CHECK(music_http_validate_response(206, &headers, 4, 9) == MUSIC_HTTP_ACCEPT);
    CHECK(music_http_validate_response(206, &headers, 3, 9) == MUSIC_HTTP_REJECT);
    CHECK(music_http_validate_response(206, &headers, 4, 10) == MUSIC_HTTP_REJECT);
    CHECK(music_http_validate_response(206, &headers, 9, 9) == MUSIC_HTTP_REJECT);
    headers = response("4", "bytes 4-7/9");
    CHECK(music_http_validate_response(206, &headers, 4, 9) == MUSIC_HTTP_REJECT);
    headers = response("5", NULL);
    CHECK(music_http_validate_response(206, &headers, 4, 9) == MUSIC_HTTP_REJECT);
    headers = response("9", NULL);
    CHECK(music_http_validate_response(200, &headers, 4, 9) == MUSIC_HTTP_RESTART_REQUIRED);
}

static void response_statuses(void)
{
    const int rejected[] = {0, 201, 204, 301, 302, 303, 307, 308, 400, 401, 403, 404, 416, 600};
    const int retried[] = {408, 429, 500, 502, 503, 504, 599};
    music_http_headers_t headers = response("9", NULL);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        CHECK(music_http_validate_response(rejected[i], &headers, 0, 9) == MUSIC_HTTP_REJECT);
    }
    for (size_t i = 0; i < sizeof(retried) / sizeof(retried[0]); i++) {
        CHECK(music_http_validate_response(retried[i], &headers, 0, 9) == MUSIC_HTTP_RETRY);
    }
    CHECK(music_http_validate_response(200, NULL, 0, 9) == MUSIC_HTTP_REJECT);
}

static void recovery_budget(void)
{
    music_http_recovery_t recovery = {0};
    music_http_recovery_begin(&recovery, 100);
    CHECK(!music_http_recovery_expired(&recovery, 100));
    CHECK(music_http_recovery_retry(&recovery, 101));
    CHECK(music_http_recovery_retry(&recovery, 102));
    CHECK(!music_http_recovery_retry(&recovery, 103));
    CHECK(recovery.retries == 2);
    recovery = (music_http_recovery_t){0};
    music_http_recovery_begin(&recovery, 100);
    music_http_recovery_begin(&recovery, 200); /* Do not silently extend deadline. */
    CHECK(recovery.started_ms == 100);
    CHECK(!music_http_recovery_expired(&recovery, 10099));
    CHECK(music_http_recovery_expired(&recovery, 10100));
    CHECK(!music_http_recovery_retry(&recovery, 10100));
    CHECK(music_http_recovery_expired(&recovery, 99));
    CHECK(!music_http_recovery_retry(NULL, 0));
}

static void recovery_progress(void)
{
    music_http_recovery_t recovery = {0};
    CHECK(music_http_recovery_retry(&recovery, 0));
    music_http_recovery_progress(&recovery); /* Useful bytes, or user's pause. */
    CHECK(!music_http_recovery_expired(&recovery, 99999));
    music_http_recovery_begin(&recovery, 100000);
    CHECK(recovery.started_ms == 100000 && recovery.retries == 1);
    CHECK(music_http_recovery_retry(&recovery, 100001));
    music_http_recovery_progress(&recovery);
    CHECK(!music_http_recovery_retry(&recovery, 200000));
    CHECK(recovery.retries == 2);
}

static void buffered_resume(void)
{
    const uint8_t stream[] = {3, 0, 'a', 'b', 'c', 2, 0, 'd', 'e'};
    uint8_t storage[4], packet[8];
    music_stream_buffer_t buffer = {0};
    music_source_t source = {0};
    music_frame_reader_t reader;
    CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
    CHECK(music_frame_reader_init(&reader, packet, sizeof(packet), 100) == MUSIC_SOURCE_OK);
    size_t offset = music_stream_buffer_write(&buffer, stream, 4), size;
    CHECK(offset == 4);
    CHECK(music_frame_reader_next(&reader, &source, 0, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(reader.packet_used == 2); /* Prefix and half-frame stay alive. */
    music_http_headers_t headers = response("5", "bytes 4-8/9");
    CHECK(music_http_validate_response(206, &headers, offset, sizeof(stream)) == MUSIC_HTTP_ACCEPT);
    size_t frames = 0;
    for (size_t step = 1; step < 10; step++) {
        offset += music_stream_buffer_write(&buffer, stream + offset, sizeof(stream) - offset);
        const music_source_result_t result = music_frame_reader_next(&reader, &source, step, &size);
        if (result == MUSIC_SOURCE_OK) {
            CHECK(size == (frames == 0 ? 3u : 2u));
            CHECK(memcmp(packet, frames == 0 ? "abc" : "de", size) == 0);
            frames++;
            if (frames == 2) {
                break;
            }
        } else {
            CHECK(result == MUSIC_SOURCE_AGAIN);
        }
    }
    CHECK(frames == 2 && offset == sizeof(stream));
    CHECK(music_stream_buffer_finish(&buffer, MUSIC_SOURCE_EOF));
    CHECK(music_frame_reader_next(&reader, &source, 10, &size) == MUSIC_SOURCE_EOF);
    music_source_close(&source);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"url_allowed", url_allowed}, {"url_rejected", url_rejected}, {"url_boundaries", url_boundaries},
        {"header_numbers", header_numbers}, {"header_ranges", header_ranges},
        {"header_duplicates", header_duplicates}, {"header_encoding", header_encoding},
        {"header_limits", header_limits}, {"response_initial", response_initial},
        {"response_resume", response_resume}, {"response_statuses", response_statuses},
        {"recovery_budget", recovery_budget}, {"recovery_progress", recovery_progress},
        {"buffered_resume", buffered_resume},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            return 0;
        }
    }
    fprintf(stderr, "Unknown case: %s\n", argv[1]);
    return 2;
}
