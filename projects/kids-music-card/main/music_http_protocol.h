#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSIC_HTTP_ORIGIN_MAX 192u
#define MUSIC_HTTP_PATH_MAX   192u
#define MUSIC_HTTP_URL_MAX    (MUSIC_HTTP_ORIGIN_MAX + MUSIC_HTTP_PATH_MAX)
#define MUSIC_HTTP_RETRIES    2u
#define MUSIC_HTTP_RECOVERY_MS 10000u

/* P0 supports an HTTPS DNS/IPv4 origin with optional port, not a URL with
 * credentials, path, query or fragment. Paths are unescaped ASCII relative
 * to this origin, beginning with one slash. All redirects are refused. */
bool music_http_build_url(const char *origin, const char *path,
                           char *url, size_t capacity);

typedef struct {
    bool length_seen;
    uint64_t length;
    bool range_seen;
    uint64_t range_start;
    uint64_t range_end;
    uint64_t range_total;
    bool encoding_seen;
    bool transfer_seen;
    bool invalid;
} music_http_headers_t;

void music_http_headers_init(music_http_headers_t *headers);
/* Call for each response header. Critical duplicates (even equal) and invalid
 * numbers are rejected. Unknown headers are not stored. No dynamic allocation. */
bool music_http_headers_add(music_http_headers_t *headers,
                             const char *name, const char *value);

typedef enum {
    MUSIC_HTTP_ACCEPT = 0,
    MUSIC_HTTP_RETRY,
    MUSIC_HTTP_RESTART_REQUIRED, /* Range ignored: never append the 200 body. */
    MUSIC_HTTP_REJECT,
} music_http_action_t;

music_http_action_t music_http_validate_response(int status,
                                                  const music_http_headers_t *headers,
                                                  uint64_t offset,
                                                  uint64_t expected_size);

typedef struct {
    uint32_t retries;
    bool recovering;
    uint64_t started_ms;
} music_http_recovery_t;

/* Two retries in TOTAL per source, not an unbounded per-read retry loop.
 * A recovery episode is at most 10 seconds. Useful byte progress clears the
 * episode timer, but never restores the retry count. Pause excludes idle time. */
void music_http_recovery_begin(music_http_recovery_t *recovery, uint64_t now_ms);
bool music_http_recovery_expired(const music_http_recovery_t *recovery, uint64_t now_ms);
bool music_http_recovery_retry(music_http_recovery_t *recovery, uint64_t now_ms);
void music_http_recovery_progress(music_http_recovery_t *recovery);
