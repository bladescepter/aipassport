#include "music_http_protocol.h"

#include <limits.h>
#include <string.h>

static size_t bounded_length(const char *text, size_t maximum)
{
    if (!text) {
        return maximum + 1;
    }
    size_t length = 0;
    while (length <= maximum && text[length]) {
        length++;
    }
    return length;
}

static bool ascii_alnum(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

static bool valid_authority(const char *host, size_t size)
{
    size_t hostname_size = size;
    for (size_t i = 0; i < size; i++) {
        if (host[i] == ':') {
            hostname_size = i;
            uint32_t port = 0;
            if (i + 1 == size || size - i - 1 > 5) {
                return false;
            }
            for (size_t j = i + 1; j < size; j++) {
                if (host[j] < '0' || host[j] > '9') {
                    return false;
                }
                port = port * 10 + (uint32_t)(host[j] - '0');
            }
            if (!port || port > 65535) {
                return false;
            }
            break;
        }
    }
    if (!hostname_size) {
        return false;
    }
    size_t label = 0;
    for (size_t i = 0; i < hostname_size; i++) {
        const char value = host[i];
        if (value == '.') {
            if (!label || host[i - 1] == '-') {
                return false;
            }
            label = 0;
        } else {
            if ((!ascii_alnum(value) && value != '-') || (!label && value == '-')) {
                return false;
            }
            if (++label > 63) {
                return false;
            }
        }
    }
    return label && host[hostname_size - 1] != '-';
}

bool music_http_build_url(const char *origin, const char *path,
                           char *url, size_t capacity)
{
    if (url && capacity) {
        url[0] = '\0';
    }
    const size_t origin_size = bounded_length(origin, MUSIC_HTTP_ORIGIN_MAX);
    const size_t path_size = bounded_length(path, MUSIC_HTTP_PATH_MAX);
    if (!url || !capacity || origin_size <= 8 || origin_size > MUSIC_HTTP_ORIGIN_MAX ||
        path_size < 2 || path_size > MUSIC_HTTP_PATH_MAX ||
        strncmp(origin, "https://", 8) != 0 ||
        !valid_authority(origin + 8, origin_size - 8) || path[0] != '/' ||
        path[path_size - 1] == '/' || origin_size + path_size >= capacity) {
        return false;
    }
    size_t segment = 1;
    for (size_t i = 1; i <= path_size; i++) {
        if (i == path_size || path[i] == '/') {
            const size_t length = i - segment;
            if (!length || (length == 1 && path[segment] == '.') ||
                (length == 2 && path[segment] == '.' && path[segment + 1] == '.')) {
                return false;
            }
            segment = i + 1;
        } else if (!ascii_alnum(path[i]) && path[i] != '-' && path[i] != '_' && path[i] != '.') {
            return false;
        }
    }
    memcpy(url, origin, origin_size);
    memcpy(url + origin_size, path, path_size + 1);
    return true;
}

static bool name_equal(const char *text, const char *expected)
{
    while (*text && *expected) {
        char value = *text++;
        if (value >= 'A' && value <= 'Z') {
            value += 'a' - 'A';
        }
        if (value != *expected++) {
            return false;
        }
    }
    return !*text && !*expected;
}

static bool value_equal(const char *value, size_t size, const char *expected)
{
    if (size != strlen(expected)) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        char character = value[i];
        if (character >= 'A' && character <= 'Z') {
            character += 'a' - 'A';
        }
        if (character != expected[i]) {
            return false;
        }
    }
    return true;
}

static void trim_value(const char **value, size_t *size)
{
    while (*size && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*size)--;
    }
    while (*size && ((*value)[*size - 1] == ' ' || (*value)[*size - 1] == '\t')) {
        (*size)--;
    }
}

static bool number(const char **value, const char *end, uint64_t *result)
{
    const char *start = *value;
    uint64_t output = 0;
    while (*value < end && **value >= '0' && **value <= '9') {
        const uint64_t digit = (uint64_t)(**value - '0');
        if (output > (UINT64_MAX - digit) / 10) {
            return false;
        }
        output = output * 10 + digit;
        (*value)++;
    }
    if (*value == start) {
        return false;
    }
    *result = output;
    return true;
}

static bool parse_range(music_http_headers_t *headers, const char *value, size_t size)
{
    if (size < 6 || !value_equal(value, 5, "bytes") || value[5] != ' ') {
        return false;
    }
    const char *cursor = value + 6;
    const char *end = value + size;
    if (!number(&cursor, end, &headers->range_start) || cursor == end || *cursor++ != '-' ||
        !number(&cursor, end, &headers->range_end) || cursor == end || *cursor++ != '/' ||
        !number(&cursor, end, &headers->range_total) || cursor != end) {
        return false;
    }
    return headers->range_total && headers->range_start <= headers->range_end &&
           headers->range_end < headers->range_total;
}

void music_http_headers_init(music_http_headers_t *headers)
{
    if (headers) {
        memset(headers, 0, sizeof(*headers));
    }
}

bool music_http_headers_add(music_http_headers_t *headers,
                             const char *name, const char *value)
{
    if (!headers || headers->invalid) {
        return false;
    }
    size_t size = bounded_length(value, 256);
    if (!name || !*name || bounded_length(name, 64) > 64 || size > 256) {
        headers->invalid = true;
        return false;
    }
    for (size_t i = 0; name[i]; i++) {
        if (!ascii_alnum(name[i]) && !strchr("!#$%&'*+-.^_`|~", name[i])) {
            headers->invalid = true;
            return false;
        }
    }
    for (size_t i = 0; i < size; i++) {
        if (((unsigned char)value[i] < 32 && value[i] != '\t') || value[i] == 127) {
            headers->invalid = true;
            return false;
        }
    }
    trim_value(&value, &size);
    bool valid = true;
    if (name_equal(name, "content-length")) {
        const char *cursor = value;
        valid = !headers->length_seen && number(&cursor, value + size, &headers->length) &&
                cursor == value + size;
        headers->length_seen = true;
    } else if (name_equal(name, "content-range")) {
        valid = !headers->range_seen && parse_range(headers, value, size);
        headers->range_seen = true;
    } else if (name_equal(name, "content-encoding")) {
        /* Only identity. Byte offsets must describe the stored Opus stream. */
        valid = !headers->encoding_seen && value_equal(value, size, "identity");
        headers->encoding_seen = true;
    } else if (name_equal(name, "transfer-encoding")) {
        /* P0 deliberately requires Content-Length, no chunked/encoded bodies. */
        valid = false;
        headers->transfer_seen = true;
    }
    headers->invalid = !valid;
    return valid;
}

music_http_action_t music_http_validate_response(int status,
                                                  const music_http_headers_t *headers,
                                                  uint64_t offset,
                                                  uint64_t expected_size)
{
    if (!headers || headers->invalid || !expected_size || expected_size > INT64_MAX ||
        offset >= expected_size) {
        return MUSIC_HTTP_REJECT;
    }
    if (status == 408 || status == 429 || (status >= 500 && status <= 599)) {
        return MUSIC_HTTP_RETRY;
    }
    if (status == 200 && offset) {
        return MUSIC_HTTP_RESTART_REQUIRED;
    }
    if (!headers->length_seen || headers->length != expected_size - offset) {
        return MUSIC_HTTP_REJECT;
    }
    if (status == 200) {
        return headers->range_seen ? MUSIC_HTTP_REJECT : MUSIC_HTTP_ACCEPT;
    }
    if (status == 206 && headers->range_seen && headers->range_start == offset &&
        headers->range_end == expected_size - 1 && headers->range_total == expected_size) {
        return MUSIC_HTTP_ACCEPT;
    }
    /* Includes 401/403/404, 416 and every redirect, even same-origin redirects. */
    return MUSIC_HTTP_REJECT;
}

void music_http_recovery_begin(music_http_recovery_t *recovery, uint64_t now_ms)
{
    if (recovery && !recovery->recovering) {
        recovery->recovering = true;
        recovery->started_ms = now_ms;
    }
}

bool music_http_recovery_expired(const music_http_recovery_t *recovery, uint64_t now_ms)
{
    return !recovery || (recovery->recovering &&
           (now_ms < recovery->started_ms || now_ms - recovery->started_ms >= MUSIC_HTTP_RECOVERY_MS));
}

bool music_http_recovery_retry(music_http_recovery_t *recovery, uint64_t now_ms)
{
    if (!recovery) {
        return false;
    }
    music_http_recovery_begin(recovery, now_ms);
    if (music_http_recovery_expired(recovery, now_ms) || recovery->retries >= MUSIC_HTTP_RETRIES) {
        return false;
    }
    recovery->retries++;
    return true;
}

void music_http_recovery_progress(music_http_recovery_t *recovery)
{
    if (recovery) {
        recovery->recovering = false;
    }
}
