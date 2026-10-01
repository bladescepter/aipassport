/* Host tests exercise the same C files linked into the firmware, without IDF. */
#include "music_file_source.h"
#include "music_frame_reader.h"
#include "music_stream_buffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) assert(expression)
#define MAX_PACKET 1500

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    size_t chunk;
    bool starved;
    music_source_result_t end;
    int cancelled;
    int closed;
} fake_transport_t;

static music_source_result_t fake_read(void *context, uint8_t *data,
                                        size_t capacity, size_t *received)
{
    fake_transport_t *fake = context;
    *received = 0;
    if (fake->starved) {
        return MUSIC_SOURCE_AGAIN;
    }
    if (fake->offset == fake->size) {
        return fake->end;
    }
    size_t size = fake->size - fake->offset;
    if (size > capacity) {
        size = capacity;
    }
    if (fake->chunk && size > fake->chunk) {
        size = fake->chunk;
    }
    memcpy(data, fake->data + fake->offset, size);
    fake->offset += size;
    *received = size;
    return MUSIC_SOURCE_OK;
}

static void fake_cancel(void *context)
{
    ((fake_transport_t *)context)->cancelled++;
}

static void fake_close(void *context)
{
    ((fake_transport_t *)context)->closed++;
}

static const music_source_ops_t FAKE_OPS = {
    .read = fake_read, .cancel = fake_cancel, .close = fake_close,
};

typedef struct {
    music_source_t source;
    music_frame_reader_t reader;
    fake_transport_t fake;
    uint8_t packet[MAX_PACKET];
} fixture_t;

static void fixture_init(fixture_t *fixture, const uint8_t *data,
                          size_t size, size_t chunk)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->fake = (fake_transport_t){
        .data = data, .size = size, .chunk = chunk, .end = MUSIC_SOURCE_EOF,
    };
    CHECK(music_source_init(&fixture->source, &FAKE_OPS, &fixture->fake) == MUSIC_SOURCE_OK);
    CHECK(music_frame_reader_init(&fixture->reader, fixture->packet,
                                  sizeof(fixture->packet), 100) == MUSIC_SOURCE_OK);
}

static music_source_result_t next_ready(fixture_t *fixture, size_t *size)
{
    for (uint64_t now = 0; now < 100; now++) {
        music_source_result_t result = music_frame_reader_next(
            &fixture->reader, &fixture->source, now, size);
        if (result != MUSIC_SOURCE_AGAIN) {
            return result;
        }
    }
    CHECK(false);
    return MUSIC_SOURCE_INVALID;
}

static void short_reads(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c', 2, 0, 'd', 'e'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 1);
    size_t size = 0;
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_OK);
    CHECK(size == 3 && memcmp(fixture.packet, "abc", 3) == 0);
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_OK);
    CHECK(size == 2 && memcmp(fixture.packet, "de", 2) == 0);
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_EOF && size == 0);
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_EOF);
    CHECK(fixture.fake.offset == sizeof(data));
    music_source_close(&fixture.source);
}

static void little_endian(void)
{
    uint8_t data[262] = {4, 1}; /* 260, not 1025 */
    memset(data + 2, 0x5a, sizeof(data) - 2);
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 31);
    size_t size;
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_OK && size == 260);
    CHECK(memcmp(fixture.packet, data + 2, size) == 0);
    music_source_close(&fixture.source);
}

static void eof_boundaries(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c'};
    for (size_t length = 0; length <= sizeof(data); length++) {
        fixture_t fixture;
        fixture_init(&fixture, data, length, 1);
        size_t size = 999;
        const music_source_result_t expected = length == 0 ? MUSIC_SOURCE_EOF :
            (length == sizeof(data) ? MUSIC_SOURCE_OK : MUSIC_SOURCE_TRUNCATED);
        CHECK(next_ready(&fixture, &size) == expected);
        if (expected != MUSIC_SOURCE_OK) {
            CHECK(size == 0);
            CHECK(next_ready(&fixture, &size) == expected);
        }
        music_source_close(&fixture.source);
    }
}

static void bad_lengths(void)
{
    const uint8_t lengths[][2] = {{0, 0}, {0xff, 0xff}, {0xdd, 5}}; /* 1501 */
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        fixture_t fixture;
        fixture_init(&fixture, lengths[i], 2, 1);
        size_t size;
        CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_BAD_FRAME);
        CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_BAD_FRAME);
        CHECK(fixture.fake.offset == 2);
        music_source_close(&fixture.source);
    }
    uint8_t maximum[MAX_PACKET + 2];
    maximum[0] = MAX_PACKET & 0xff;
    maximum[1] = MAX_PACKET >> 8;
    memset(maximum + 2, 0x7b, MAX_PACKET);
    fixture_t fixture;
    fixture_init(&fixture, maximum, sizeof(maximum), 0);
    size_t size;
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_OK && size == MAX_PACKET);
    music_source_close(&fixture.source);
}

static void starvation(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 1);
    size_t size;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 0, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(fixture.reader.header_used == 1);
    fixture.fake.starved = true;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 1, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(fixture.reader.header_used == 1);
    fixture.fake.starved = false;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 2, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(fixture.reader.packet_used == 1);
    fixture.fake.starved = true;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 3, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(fixture.reader.packet_used == 1);
    fixture.fake.starved = false;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 4, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 5, &size) == MUSIC_SOURCE_OK);
    CHECK(size == 3 && memcmp(fixture.packet, "abc", 3) == 0);
    music_source_close(&fixture.source);
}

static void deadline(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 1);
    size_t size;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 100, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 199, &size) == MUSIC_SOURCE_AGAIN);
    const size_t before = fixture.fake.offset;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 200, &size) == MUSIC_SOURCE_TIMEOUT);
    CHECK(fixture.fake.offset == before && size == 0);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 201, &size) == MUSIC_SOURCE_TIMEOUT);
    music_source_close(&fixture.source);
}

static void pause_resume(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 1);
    size_t size;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 0, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 1, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(fixture.reader.packet_used == 1);
    music_frame_reader_resume(&fixture.reader, 10000);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 10001, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 10002, &size) == MUSIC_SOURCE_OK);
    CHECK(size == 3 && memcmp(fixture.packet, "abc", 3) == 0);
    music_source_close(&fixture.source);
}

static void cancellation(void)
{
    const uint8_t data[] = {3, 0, 'a', 'b', 'c'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 1);
    size_t size;
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 0, &size) == MUSIC_SOURCE_AGAIN);
    music_source_cancel(&fixture.source);
    music_source_cancel(&fixture.source);
    CHECK(fixture.fake.cancelled == 1);
    CHECK(music_frame_reader_next(&fixture.reader, &fixture.source, 1, &size) == MUSIC_SOURCE_CANCELLED);
    CHECK(size == 0 && fixture.fake.offset == 1);
    music_source_close(&fixture.source);
    music_source_close(&fixture.source);
    CHECK(fixture.fake.closed == 1);
    /* A new generation starts with fresh parser and transport state. */
    fixture_init(&fixture, data, sizeof(data), 0);
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_OK && size == 3);
    music_source_close(&fixture.source);
}

static void transport_error(void)
{
    const uint8_t data[] = {3, 0, 'a'};
    fixture_t fixture;
    fixture_init(&fixture, data, sizeof(data), 0);
    fixture.fake.end = MUSIC_SOURCE_IO_ERROR;
    size_t size;
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_IO_ERROR && size == 0);
    CHECK(next_ready(&fixture, &size) == MUSIC_SOURCE_IO_ERROR);
    music_source_close(&fixture.source);
}

static music_source_result_t invalid_read(void *context, uint8_t *data,
                                           size_t capacity, size_t *received)
{
    (void)data;
    switch (*(int *)context) {
    case 0: *received = 0; return MUSIC_SOURCE_OK;
    case 1: *received = capacity + 1; return MUSIC_SOURCE_OK;
    case 2: *received = 1; return MUSIC_SOURCE_EOF;
    case 3: *received = 0; return (music_source_result_t)99;
    default: *received = 0; return (music_source_result_t)-99;
    }
}

static void source_contract(void)
{
    music_source_t source = {0};
    const music_source_ops_t ops = {.read = invalid_read};
    uint8_t data[2];
    size_t size = 1;
    CHECK(music_source_read(&source, data, sizeof(data), &size) == MUSIC_SOURCE_INVALID && size == 0);
    CHECK(music_source_init(NULL, &ops, NULL) == MUSIC_SOURCE_INVALID);
    CHECK(music_source_init(&source, NULL, NULL) == MUSIC_SOURCE_INVALID);
    for (int scenario = 0; scenario < 5; scenario++) {
        CHECK(music_source_init(&source, &ops, &scenario) == MUSIC_SOURCE_OK);
        CHECK(music_source_init(&source, &ops, &scenario) == MUSIC_SOURCE_INVALID);
        CHECK(music_source_read(&source, data, sizeof(data), &size) == MUSIC_SOURCE_INVALID);
        CHECK(size == 0);
        CHECK(music_source_read(&source, data, 0, &size) == MUSIC_SOURCE_INVALID);
        music_source_close(&source);
    }
    music_frame_reader_t reader = {0};
    CHECK(music_frame_reader_init(&reader, data, 0, 1) == MUSIC_SOURCE_INVALID);
    CHECK(music_frame_reader_init(&reader, data, sizeof(data), 0) == MUSIC_SOURCE_INVALID);
    CHECK(music_frame_reader_next(&reader, &source, 0, &size) == MUSIC_SOURCE_INVALID);
}

static void ring_wrap(void)
{
    uint8_t storage[5], output[8];
    const uint8_t first[] = {1, 2, 3, 4, 5, 6};
    const uint8_t second[] = {6, 7, 8};
    const uint8_t expected[] = {4, 5, 6, 7, 8};
    music_stream_buffer_t buffer = {0};
    music_source_t source = {0};
    CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
    size_t size;
    CHECK(music_source_read(&source, output, sizeof(output), &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_stream_buffer_write(&buffer, first, sizeof(first)) == 5);
    CHECK(music_stream_buffer_space(&buffer) == 0);
    CHECK(music_stream_buffer_write(&buffer, second, 1) == 0);
    CHECK(music_source_read(&source, output, 3, &size) == MUSIC_SOURCE_OK && size == 3);
    CHECK(memcmp(output, first, 3) == 0);
    CHECK(music_stream_buffer_write(&buffer, second, sizeof(second)) == 3);
    CHECK(music_source_read(&source, output, sizeof(output), &size) == MUSIC_SOURCE_OK && size == 5);
    CHECK(memcmp(output, expected, 5) == 0);
    CHECK(music_stream_buffer_space(&buffer) == sizeof(storage));
    CHECK(music_source_read(&source, output, sizeof(output), &size) == MUSIC_SOURCE_AGAIN);
    music_source_close(&source);
}

static void ring_terminal(void)
{
    const music_source_result_t endings[] = {
        MUSIC_SOURCE_EOF, MUSIC_SOURCE_IO_ERROR, MUSIC_SOURCE_TIMEOUT,
    };
    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); i++) {
        uint8_t storage[8], output[8];
        const uint8_t data[] = {1, 0, 'a'};
        music_stream_buffer_t buffer = {0};
        music_source_t source = {0};
        CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
        CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
        CHECK(!music_stream_buffer_finish(&buffer, MUSIC_SOURCE_OK));
        CHECK(music_stream_buffer_write(&buffer, data, sizeof(data)) == sizeof(data));
        CHECK(music_stream_buffer_finish(&buffer, endings[i]));
        CHECK(!music_stream_buffer_finish(&buffer, MUSIC_SOURCE_EOF));
        CHECK(music_stream_buffer_write(&buffer, data, sizeof(data)) == 0);
        size_t size;
        CHECK(music_source_read(&source, output, sizeof(output), &size) == MUSIC_SOURCE_OK);
        CHECK(size == sizeof(data) && memcmp(output, data, size) == 0);
        CHECK(music_source_read(&source, output, sizeof(output), &size) == endings[i]);
        CHECK(size == 0);
        music_source_close(&source);
    }
    uint8_t storage[8], output[8];
    music_stream_buffer_t buffer = {0};
    music_source_t source = {0};
    CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_write(&buffer, (const uint8_t *)"old", 3) == 3);
    music_source_cancel(&source);
    size_t size;
    CHECK(buffer.used == 0);
    CHECK(music_source_read(&source, output, sizeof(output), &size) == MUSIC_SOURCE_CANCELLED);
    CHECK(music_stream_buffer_write(&buffer, (const uint8_t *)"new", 3) == 0);
    music_source_close(&source);
}

static void ring_model(void)
{
    uint8_t storage[7], input[11], output[11];
    music_stream_buffer_t buffer = {0};
    music_source_t source = {0};
    CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
    size_t written = 0, read = 0;
    uint32_t random = 17;
    for (size_t step = 0; step < 10000; step++) {
        random = random * 1664525u + 1013904223u;
        const size_t requested = 1 + (random >> 8) % sizeof(input);
        if (random & 0x10000u) {
            for (size_t i = 0; i < requested; i++) {
                input[i] = (uint8_t)(written + i);
            }
            const size_t accepted = music_stream_buffer_write(&buffer, input, requested);
            CHECK(accepted <= requested);
            written += accepted;
        } else {
            size_t size;
            const music_source_result_t result = music_source_read(&source, output, requested, &size);
            CHECK(result == (written == read ? MUSIC_SOURCE_AGAIN : MUSIC_SOURCE_OK));
            for (size_t i = 0; i < size; i++) {
                CHECK(output[i] == (uint8_t)(read + i));
            }
            read += size;
        }
        CHECK(buffer.used == written - read && buffer.used <= sizeof(storage));
        CHECK(music_stream_buffer_space(&buffer) + buffer.used == sizeof(storage));
    }
    CHECK(music_stream_buffer_finish(&buffer, MUSIC_SOURCE_EOF));
    for (;;) {
        size_t size;
        music_source_result_t result = music_source_read(&source, output, sizeof(output), &size);
        if (result == MUSIC_SOURCE_EOF) {
            break;
        }
        CHECK(result == MUSIC_SOURCE_OK);
        for (size_t i = 0; i < size; i++) {
            CHECK(output[i] == (uint8_t)(read + i));
        }
        read += size;
    }
    CHECK(read == written);
    music_source_close(&source);
}

static void buffered_frames(void)
{
    uint8_t storage[4], packet[8];
    const uint8_t data[] = {3, 0, 'a', 'b', 'c', 1, 0, 'd'};
    music_stream_buffer_t buffer = {0};
    music_source_t source = {0};
    music_frame_reader_t reader;
    CHECK(music_stream_buffer_init(&buffer, storage, sizeof(storage)) == MUSIC_SOURCE_OK);
    CHECK(music_stream_buffer_attach(&source, &buffer) == MUSIC_SOURCE_OK);
    CHECK(music_frame_reader_init(&reader, packet, sizeof(packet), 100) == MUSIC_SOURCE_OK);
    size_t frames = 0, size;
    for (size_t i = 0; i < sizeof(data); i++) {
        CHECK(music_stream_buffer_write(&buffer, data + i, 1) == 1);
        music_source_result_t result = music_frame_reader_next(&reader, &source, i, &size);
        if (result == MUSIC_SOURCE_OK) {
            CHECK(size == (frames == 0 ? 3u : 1u));
            CHECK(memcmp(packet, frames == 0 ? "abc" : "d", size) == 0);
            frames++;
        } else {
            CHECK(result == MUSIC_SOURCE_AGAIN && size == 0);
        }
    }
    CHECK(frames == 2);
    CHECK(music_stream_buffer_finish(&buffer, MUSIC_SOURCE_EOF));
    CHECK(music_frame_reader_next(&reader, &source, 10, &size) == MUSIC_SOURCE_EOF);
    music_source_close(&source);
}

static void file_source(const char *path)
{
    FILE *file = fopen(path, "wb");
    const uint8_t data[] = {3, 0, 'a', 'b', 'c', 2}; /* truncated next header */
    CHECK(file && fwrite(data, 1, sizeof(data), file) == sizeof(data));
    CHECK(fclose(file) == 0);
    music_file_source_t context = {0};
    music_source_t source = {0};
    uint8_t packet[8];
    music_frame_reader_t reader;
    CHECK(music_file_source_open(&source, &context, path) == MUSIC_SOURCE_OK);
    CHECK(music_file_source_open(&source, &context, path) == MUSIC_SOURCE_INVALID);
    CHECK(music_frame_reader_init(&reader, packet, sizeof(packet), 100) == MUSIC_SOURCE_OK);
    size_t size;
    CHECK(music_frame_reader_next(&reader, &source, 0, &size) == MUSIC_SOURCE_OK);
    CHECK(size == 3 && memcmp(packet, "abc", 3) == 0);
    CHECK(music_frame_reader_next(&reader, &source, 1, &size) == MUSIC_SOURCE_AGAIN);
    CHECK(music_frame_reader_next(&reader, &source, 2, &size) == MUSIC_SOURCE_TRUNCATED);
    music_source_close(&source);
    CHECK(!context.file);
    music_source_close(&source);
    CHECK(music_file_source_open(&source, &context, "") == MUSIC_SOURCE_INVALID);
    /* A regular file cannot be a parent directory, so this is deterministic. */
    char missing[4096];
    CHECK(snprintf(missing, sizeof(missing), "%s/missing", path) < (int)sizeof(missing));
    CHECK(music_file_source_open(&source, &context, missing) == MUSIC_SOURCE_IO_ERROR);
    CHECK(!context.file && !source.ops);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"short_reads", short_reads}, {"little_endian", little_endian},
        {"eof_boundaries", eof_boundaries}, {"bad_lengths", bad_lengths},
        {"starvation", starvation}, {"deadline", deadline},
        {"pause_resume", pause_resume}, {"cancellation", cancellation},
        {"transport_error", transport_error}, {"source_contract", source_contract},
        {"ring_wrap", ring_wrap}, {"ring_terminal", ring_terminal},
        {"ring_model", ring_model}, {"buffered_frames", buffered_frames},
    };
    if (strcmp(argv[1], "file_source") == 0) {
        file_source(argv[2]);
        return 0;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            return 0;
        }
    }
    fprintf(stderr, "Unknown case: %s\n", argv[1]);
    return 2;
}
