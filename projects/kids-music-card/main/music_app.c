// Offline children's music player for the FoloToy AI Passport.
//
// The input task owns all application state and LVGL objects.  Button callbacks,
// the audio worker and the blank-screen timer only enqueue small events.  The
// audio worker is the sole owner of blocking file/Opus/I2S operations.
#include "music_app.h"

#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "music_catalog.h"
#include "music_library.h"
#include <stdlib.h>
#include "music_file_source.h"
#include "music_frame_reader.h"
#include "music_font_18.h"
#include "nvs.h"
#include "opus.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_MUSIC_ONLINE_P0
#include "music_http_source.h"
#include "music_online_profile.h"
#include "music_network.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "music_app";

/* Created once before Wi-Fi/TLS allocation; shared by every song. */
static OpusDecoder *s_decoder;

#define MUSIC_FS_PARTITION       "musicfs"
#define MUSIC_FS_MOUNT           "/music"
#define MUSIC_SAMPLE_RATE        16000u
#define MUSIC_CHANNELS           1u
#define MUSIC_BITS               16u
#define OPUS_FRAME_SAMPLES       960       // 60 ms at 16 kHz, the decoder maximum
#define OPUS_MAX_PACKET          1500      // larger than a normal 32 kbps music frame
#define FRAME_TIMEOUT_MS         10000u   // bounded assembly; not a network retry policy
#define AUTO_BLANK_US            (5LL * 1000LL * 1000LL)
#define MENU_VISIBLE_ROWS        6
#define VOLUME_MIN                1
#define VOLUME_MAX                10
#define DEFAULT_VOLUME            8
#define APP_QUEUE_DEPTH           24
#define PLAYER_QUEUE_DEPTH        8
#define SETTINGS_QUEUE_DEPTH      4

/* A deliberately small, high-contrast palette suitable for a child-facing UI. */
#define COLOR_BG                  0x101827
#define COLOR_TOP                 0x172743
#define COLOR_PANEL               0x1B2A43
#define COLOR_SELECTED            0x2E62A5
#define COLOR_TEXT                0xF5F8FF
#define COLOR_MUTED               0xAAB8D1
#define COLOR_ACCENT              0xFFD166
#define COLOR_ERROR               0xFF8A8A

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* -------------------------------------------------------------------------- */
/* Application and player messages                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    MUSIC_MODE_SEQUENTIAL = 0,
    MUSIC_MODE_RANDOM,
    MUSIC_MODE_SINGLE,
    MUSIC_MODE_COUNT,
} music_mode_t;

typedef enum {
    PAGE_MENU = 0,
    PAGE_LOCAL, PAGE_ONLINE, PAGE_CACHE, PAGE_SETTINGS,
    PAGE_VOLUME, PAGE_WIFI, PAGE_CONFIRM, PAGE_BUSY,
    PAGE_SCAN, PAGE_AUTH, PAGE_KEYBOARD,
    PAGE_MODE, PAGE_PLAYBACK,
} page_t;

typedef enum {
    APP_EVENT_BUTTON = 0,
    APP_EVENT_PLAYER_FINISHED,
    APP_EVENT_PLAYER_ERROR,
    APP_EVENT_BLANK,
    APP_EVENT_LIBRARY,
    APP_EVENT_PLAYER_STOPPED,
    APP_EVENT_TEST_REPORT,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    int song;
    int error;
    uint32_t generation;
    uint32_t blank_cookie;
    music_library_event_t *library;
} app_event_t;

typedef enum {
    PLAYER_CMD_START = 0,
    PLAYER_CMD_STOP,
    PLAYER_CMD_PAUSE,
    PLAYER_CMD_RESUME,
    PLAYER_CMD_SET_VOLUME,
} player_command_type_t;

typedef struct {
    player_command_type_t type;
    int song;
    uint8_t volume;
    uint32_t generation;
    music_item_t item;
    bool paused;
} player_command_t;

typedef struct {
    uint8_t mode;
    uint8_t volume;
    bool immediate;
} settings_update_t;

static page_t s_page = PAGE_MENU;
static music_mode_t s_mode = MUSIC_MODE_SEQUENTIAL;
static music_mode_t s_mode_selection = MUSIC_MODE_SEQUENTIAL;
static int s_menu_selected;
static int s_menu_top;
static int s_current_song = -1;
static bool s_playing;
static bool s_paused;
static int s_play_error;
static bool s_screen_awake = true;
static int64_t s_ignore_ok_until_us;
static bool s_fs_mounted;
static bool s_started;
static uint8_t s_volume = DEFAULT_VOLUME;
static uint32_t s_play_generation;
static uint32_t s_blank_cookie;

static uint16_t s_shuffle[MUSIC_REMOTE_MAX];
static int s_shuffle_position = -1;
static music_item_t s_local[MUSIC_LOCAL_MAX];
static music_item_t *s_remote;
static music_library_event_t *s_remote_owner;
static void fetch_page_locked(page_t page, int number, bool refresh);
static music_item_t s_selected[MUSIC_CACHE_MAX], s_current_item;
static int s_local_count, s_remote_count, s_remote_total, s_remote_page, s_selected_count;
static char s_release[MUSIC_RELEASE_BYTES], s_notice[96];
static uint32_t s_library_generation;
static bool s_play_online, s_loading;
static uint8_t s_volume_edit;
static int s_pending_action; /* 1=cache apply, 2=provision after audio STOP acknowledgement */

static QueueHandle_t s_app_queue;
static QueueHandle_t s_player_queue;
static QueueHandle_t s_settings_queue;
static TaskHandle_t s_input_task;
static TaskHandle_t s_player_task;
static TaskHandle_t s_settings_task;
static esp_timer_handle_t s_blank_timer;

#define PLAYER_STACK_BYTES 16384
static StackType_t s_player_stack[PLAYER_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_player_tcb;

/* One screen is active at a time.  Only the input task accesses these objects. */
static lv_obj_t *s_screen;
static lv_obj_t *s_header;
static lv_obj_t *s_footer;
static lv_obj_t *s_menu_rows[MENU_VISIBLE_ROWS];
static lv_obj_t *s_menu_labels[MENU_VISIBLE_ROWS];
static lv_obj_t *s_mode_rows[MUSIC_MODE_COUNT + 1];
static lv_obj_t *s_mode_labels[MUSIC_MODE_COUNT + 1];
static lv_obj_t *s_play_title;
static lv_obj_t *s_play_status;
static lv_obj_t *s_play_volume;
static lv_obj_t *s_play_mode;

static const char *const s_mode_names[MUSIC_MODE_COUNT] = {
    "顺序播放",
    "随机播放",
    "单曲循环",
};

static bool is_online_song(int song) { return song >= MUSIC_LOCAL_MAX; }
static int song_count(void) { return MUSIC_LOCAL_MAX + MUSIC_REMOTE_MAX; }

static void library_callback(music_library_event_t *library)
{
    const app_event_t event = {.type = APP_EVENT_LIBRARY, .library = library};
    ESP_LOGI(TAG, "lib callback kind=%d gen=%u", library ? (int)library->kind : -1, library ? library->generation : 0);
    if (!s_app_queue || xQueueSend(s_app_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "lib event dropped before queue");
        free(library);
    }
}

/* -------------------------------------------------------------------------- */
/* Small cross-task event helpers                                             */
/* -------------------------------------------------------------------------- */

static void post_app_event(const app_event_t *event)
{
    if (!s_app_queue || !event) {
        return;
    }
    if (xQueueSend(s_app_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "应用事件队列已满，丢弃事件 %d", (int)event->type);
    }
}

static void post_player_event(app_event_type_t type, int song,
                              uint32_t generation, int error)
{
    const app_event_t event = {
        .type = type,
        .song = song,
        .generation = generation,
        .error = error,
    };
    post_app_event(&event);
}

static bool send_player_command(player_command_type_t type, int song,
                                uint8_t volume, uint32_t generation)
{
    if (!s_player_queue) {
        ESP_LOGE(TAG, "播放器队列未就绪");
        return false;
    }
    const player_command_t command = {
        .type = type,
        .song = song,
        .volume = volume,
        .generation = generation,
        .item = s_current_item,
        .paused = s_paused,
    };
    if (xQueueSend(s_player_queue, &command, 0) != pdTRUE) {
        ESP_LOGE(TAG, "播放器队列已满，命令 %d 未发送", (int)type);
        return false;
    }
    return true;
}

static void post_settings(bool immediate)
{
    if (!s_settings_queue) {
        return;
    }
    const settings_update_t update = {
        .mode = (uint8_t)s_mode,
        .volume = s_volume,
        .immediate = immediate,
    };
    if (xQueueSend(s_settings_queue, &update, 0) != pdTRUE) {
        ESP_LOGW(TAG, "设置队列已满，稍后再次修改时重试保存");
    }
}

/* -------------------------------------------------------------------------- */
/* Resource and settings initialization                                      */
/* -------------------------------------------------------------------------- */

static bool mount_music_fs(void)
{
    const esp_vfs_spiffs_conf_t config = {
        .base_path = MUSIC_FS_MOUNT,
        .partition_label = MUSIC_FS_PARTITION,
        .max_files = 4,
        // Never erase a user's data partition just because it is unavailable.
        .format_if_mount_failed = false,
    };
    const esp_err_t error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "音乐分区挂载失败: %s", esp_err_to_name(error));
        return false;
    }
    s_fs_mounted = true;
    ESP_LOGI(TAG, "音乐分区已挂载: %s", MUSIC_FS_MOUNT);
    return true;
}

static void load_settings(void)
{
    s_mode = MUSIC_MODE_SEQUENTIAL;
    s_volume = DEFAULT_VOLUME;

    nvs_handle_t handle;
    if (nvs_open("music", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t mode = 0;
    uint8_t volume = DEFAULT_VOLUME;
    if (nvs_get_u8(handle, "mode", &mode) == ESP_OK && mode < MUSIC_MODE_COUNT) {
        s_mode = (music_mode_t)mode;
    }
    if (nvs_get_u8(handle, "volume", &volume) == ESP_OK &&
        volume >= VOLUME_MIN && volume <= VOLUME_MAX) {
        s_volume = volume;
    }
    nvs_close(handle);
}

static void settings_worker(void *argument)
{
    (void)argument;
    nvs_handle_t handle = 0;
    const esp_err_t open_error = nvs_open("music", NVS_READWRITE, &handle);
    if (open_error != ESP_OK) {
        ESP_LOGE(TAG, "NVS 设置命名空间打开失败: %s", esp_err_to_name(open_error));
    }

    settings_update_t update;
    for (;;) {
        if (xQueueReceive(s_settings_queue, &update, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Coalesce volume key presses.  Mode confirmation is marked
        // immediate so a quick power-off after choosing a mode still saves it.
        if (!update.immediate) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        settings_update_t newer;
        while (xQueueReceive(s_settings_queue, &newer, 0) == pdTRUE) {
            update = newer;
        }

        if (open_error != ESP_OK) {
            continue;
        }
        esp_err_t error = nvs_set_u8(handle, "mode", update.mode);
        if (error == ESP_OK) {
            error = nvs_set_u8(handle, "volume", update.volume);
        }
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "NVS 设置保存失败: %s", esp_err_to_name(error));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Opus streaming player                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    music_source_t source;
    music_file_source_t file;
    music_frame_reader_t frame_reader;
    uint8_t packet[OPUS_MAX_PACKET];
    OpusDecoder *decoder;
    int song;
    uint32_t generation;
    bool playing;
    bool paused;
} player_state_t;

static void player_close(player_state_t *state)
{
    if (!state) {
        return;
    }
    /* The decoder is allocated once at startup and reused for every song: a
     * per-song allocation can fail on a fragmented heap (OPUS_ALLOC_FAIL -7)
     * even when total free memory is sufficient. */
    music_source_cancel(&state->source);
    music_source_close(&state->source);
    state->playing = false;
    state->paused = false;
    state->song = -1;
}

static bool player_open(player_state_t *state, const player_command_t *command)
{
    if (!state || !command || command->song < 0 || command->song >= song_count()) {
        return false;
    }
    const char *title = command->item.title;
#if CONFIG_MUSIC_ONLINE_P0
    if (is_online_song(command->song)) {
        music_online_profile_t profile;
        esp_err_t error = music_online_profile_load(&profile);
        if (error == ESP_OK) {
            char remote[193];
            music_item_remote_path(&command->item, remote, sizeof(remote));
            const music_http_source_config_t config = {
                .origin = profile.origin,
                .audio_path = remote,
                .username = profile.username,
                .password = profile.password,
                .expected_size = command->item.size,
                .buffer_bytes = CONFIG_MUSIC_ONLINE_BUFFER_BYTES,
                .start_bytes = CONFIG_MUSIC_ONLINE_START_BYTES,
            };
            error = music_http_source_open(&state->source, &config);
        }
        music_online_profile_clear(&profile);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "P0 source/profile unavailable: %s", esp_err_to_name(error));
            return false;
        }
    } else
#endif
    {
        if (!s_fs_mounted) {
            return false;
        }
        const char *path = command->item.local;
        if (strncmp(path, "/music/", 7) || strstr(path, "..")) return false;
        if (music_file_source_open(&state->source, &state->file, path) != MUSIC_SOURCE_OK) {
            ESP_LOGE(TAG, "找不到歌曲资源: %s (%s)", title, path);
            return false;
        }
    }
    if (music_frame_reader_init(&state->frame_reader, state->packet,
                                sizeof(state->packet), FRAME_TIMEOUT_MS) != MUSIC_SOURCE_OK) {
        music_source_close(&state->source);
        return false;
    }

    if (bsp_audio_set_format(MUSIC_SAMPLE_RATE, MUSIC_BITS, MUSIC_CHANNELS) != ESP_OK) {
        ESP_LOGE(TAG, "音频格式初始化失败: %s", title);
        music_source_close(&state->source);
        return false;
    }

    if (!s_decoder || opus_decoder_ctl(s_decoder, OPUS_RESET_STATE) != OPUS_OK) {
        ESP_LOGE(TAG, "Opus 解码器不可用: %s", title);
        music_source_close(&state->source);
        return false;
    }

    bsp_audio_set_volume(command->volume * 10u);
    state->decoder = s_decoder;
    state->song = command->song;
    state->generation = command->generation;
    state->playing = true;
    state->paused = command->paused;
#if CONFIG_MUSIC_ONLINE_P0
    music_http_source_set_paused(&state->source, command->paused);
#endif
    ESP_LOGI(TAG, "开始播放: %s", title);
    return true;
}

static void player_handle_command(player_state_t *state,
                                  const player_command_t *command)
{
    if (!state || !command) {
        return;
    }

    switch (command->type) {
    case PLAYER_CMD_START:
        player_close(state);
        if (!player_open(state, command)) {
            post_player_event(APP_EVENT_PLAYER_ERROR, command->song,
                              command->generation, ESP_FAIL);
        }
        break;
    case PLAYER_CMD_STOP:
        player_close(state);
        post_player_event(APP_EVENT_PLAYER_STOPPED, -1, command->generation, ESP_OK);
        break;
    case PLAYER_CMD_PAUSE:
        if (state->playing) {
            state->paused = true;
#if CONFIG_MUSIC_ONLINE_P0
            music_http_source_set_paused(&state->source, true);
#endif
        }
        break;
    case PLAYER_CMD_RESUME:
        if (state->playing) {
            state->paused = false;
#if CONFIG_MUSIC_ONLINE_P0
            music_http_source_set_paused(&state->source, false);
#endif
            music_frame_reader_resume(&state->frame_reader,
                                      (uint64_t)(esp_timer_get_time() / 1000));
        }
        break;
    case PLAYER_CMD_SET_VOLUME:
        bsp_audio_set_volume(command->volume * 10u);
        break;
    default:
        break;
    }
}

static void player_worker(void *argument)
{
    (void)argument;
    player_state_t state = {0};
    state.song = -1;

    int16_t pcm[OPUS_FRAME_SAMPLES];

    for (;;) {
        if (!state.playing || state.paused) {
            player_command_t command;
            if (xQueueReceive(s_player_queue, &command, portMAX_DELAY) == pdTRUE) {
                player_handle_command(&state, &command);
            }
            continue;
        }

        // Process commands between frames and partial-read attempts. Network
        // backends must read buffered bytes here, never block on TLS/HTTP.
        player_command_t command;
        while (xQueueReceive(s_player_queue, &command, 0) == pdTRUE) {
            player_handle_command(&state, &command);
            if (!state.playing || state.paused) {
                break;
            }
        }
        if (!state.playing || state.paused) {
            continue;
        }

        size_t packet_length = 0;
        const music_source_result_t result = music_frame_reader_next(
            &state.frame_reader, &state.source,
            (uint64_t)(esp_timer_get_time() / 1000), &packet_length);
        if (result == MUSIC_SOURCE_AGAIN) {
            // Temporary starvation is neither EOF nor a reason to busy-spin.
#if CONFIG_MUSIC_ONLINE_P0
            if (is_online_song(state.song)) {
                music_http_source_wait(&state.source);
            } else
#endif
            {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }
        if (result != MUSIC_SOURCE_OK) {
            const int song = state.song;
            const uint32_t generation = state.generation;
#if CONFIG_MUSIC_ONLINE_P0
            const music_http_failure_t http_failure = music_http_source_failure(&state.source);
#endif
            player_close(&state);
            if (result == MUSIC_SOURCE_EOF) {
                ESP_LOGI(TAG, "Playback completed: song=%d", song);
                post_player_event(APP_EVENT_PLAYER_FINISHED, song, generation, ESP_OK);
            } else {
                int error = result == MUSIC_SOURCE_TIMEOUT ? ESP_ERR_TIMEOUT :
                    (result == MUSIC_SOURCE_TRUNCATED || result == MUSIC_SOURCE_BAD_FRAME
                     ? ESP_ERR_INVALID_SIZE : ESP_FAIL);
#if CONFIG_MUSIC_ONLINE_P0
                if (http_failure == MUSIC_HTTP_FAILURE_RESTART_REQUIRED) {
                    error = ESP_ERR_INVALID_RESPONSE;
                }
#endif
                ESP_LOGE(TAG, "歌曲 %d 的音频流读取失败: %d", song, (int)result);
                post_player_event(APP_EVENT_PLAYER_ERROR, song, generation, error);
            }
            continue;
        }

        const int samples = opus_decode(state.decoder, state.packet, (opus_int32)packet_length,
                                        pcm, OPUS_FRAME_SAMPLES, 0);
        if (samples < 0) {
            const int song = state.song;
            const uint32_t generation = state.generation;
            ESP_LOGE(TAG, "歌曲 %d 的 Opus 解码失败: %d", song, samples);
            player_close(&state);
            post_player_event(APP_EVENT_PLAYER_ERROR, song, generation, samples);
            continue;
        }
        if (bsp_audio_write(pcm, (size_t)samples * sizeof(pcm[0])) != ESP_OK) {
            const int song = state.song;
            const uint32_t generation = state.generation;
            ESP_LOGE(TAG, "歌曲 %d 的 PCM 输出失败", song);
            player_close(&state);
            post_player_event(APP_EVENT_PLAYER_ERROR, song, generation, ESP_FAIL);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* UI helpers                                                                 */
/* -------------------------------------------------------------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    static lv_anim_t readable_scroll;
    static bool scroll_initialized;
    if (!scroll_initialized) {
        lv_anim_init(&readable_scroll);
        lv_anim_set_delay(&readable_scroll, 3000);
        lv_anim_set_reverse_delay(&readable_scroll, 3000);
        lv_anim_set_repeat_delay(&readable_scroll, 3000);
        lv_anim_set_repeat_count(&readable_scroll, LV_ANIM_REPEAT_INFINITE);
        scroll_initialized = true;
    }
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_anim(label, &readable_scroll, 0);
    lv_obj_set_style_anim_duration(label, lv_anim_speed(15), 0);
    lv_obj_set_style_text_font(label, &music_font_18, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_screen(const char *title)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    s_header = lv_obj_create(screen);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_size(s_header, 240, 42);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_header, lv_color_hex(COLOR_TOP), 0);
    lv_obj_set_style_border_width(s_header, 0, 0);
    lv_obj_set_style_pad_all(s_header, 0, 0);

    lv_obj_t *header_label = make_label(s_header, title, lv_color_hex(COLOR_TEXT));
    lv_obj_set_pos(header_label, 10, 0);
    lv_obj_set_size(header_label, 220, 42);
    lv_obj_set_style_text_align(header_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(header_label, lv_color_hex(COLOR_ACCENT), 0);

    return screen;
}

static void make_footer(lv_obj_t *screen, const char *text)
{
    s_footer = lv_obj_create(screen);
    lv_obj_set_pos(s_footer, 0, 290);
    lv_obj_set_size(s_footer, 240, 30);
    lv_obj_remove_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_footer, lv_color_hex(COLOR_TOP), 0);
    lv_obj_set_style_border_width(s_footer, 0, 0);
    lv_obj_set_style_pad_all(s_footer, 0, 0);

    lv_obj_t *label = make_label(s_footer, text, lv_color_hex(COLOR_MUTED));
    lv_obj_set_size(label, 240, 30);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
}

static lv_obj_t *make_row(lv_obj_t *screen, int y, const char *text, bool selected,
                          lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_size(row, 232, 38);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row,
                              selected ? lv_color_hex(COLOR_SELECTED)
                                       : lv_color_hex(COLOR_PANEL), 0);

    lv_obj_t *label = make_label(row, text, lv_color_hex(COLOR_TEXT));
    lv_obj_set_pos(label, 10, 0);
    lv_obj_set_size(label, 214, 38);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, selected ? LV_LABEL_LONG_SCROLL
                                           : LV_LABEL_LONG_CLIP);
    if (label_out) {
        *label_out = label;
    }
    return row;
}

static void destroy_screen_locked(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
    }
    s_screen = NULL;
    s_header = NULL;
    s_footer = NULL;
    memset(s_menu_rows, 0, sizeof(s_menu_rows));
    memset(s_menu_labels, 0, sizeof(s_menu_labels));
    memset(s_mode_rows, 0, sizeof(s_mode_rows));
    memset(s_mode_labels, 0, sizeof(s_mode_labels));
    s_play_title = NULL;
    s_play_status = NULL;
    s_play_volume = NULL;
    s_play_mode = NULL;
}

static char s_wifi_names[12][33], s_wifi_name[33], s_account_name[65] = "kidmusic";
static int s_wifi_count = -1, s_key_group, s_edit_field;
static int64_t s_reveal_until;
static char s_edit_text[129];
static lv_obj_t *s_key_labels[40], *s_key_text;
static const char *s_key_groups[] = {"0123456789", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"};
static void build_list_locked(page_t page);
static void begin_edit(int field)
{
    s_edit_field = field; s_key_group = field == 3 ? 0 : 1; s_reveal_until = 0;
    memset(s_edit_text, 0, sizeof(s_edit_text));
    if (field == 2) snprintf(s_edit_text, sizeof(s_edit_text), "%s", s_account_name);
    s_notice[0] = 0;
    build_list_locked(PAGE_KEYBOARD);
}

static const char *display_title(const music_item_t *item)
{
    const unsigned char *p = (const unsigned char *)item->title;
    while (*p) {
        uint32_t code; int remaining;
        if (*p < 128) { code = *p++; remaining = 0; }
        else if ((*p & 0xe0) == 0xc0) { code = *p++ & 0x1f; remaining = 1; }
        else if ((*p & 0xf0) == 0xe0) { code = *p++ & 0x0f; remaining = 2; }
        else if ((*p & 0xf8) == 0xf0) { code = *p++ & 7; remaining = 3; }
        else return item->id;
        while (remaining--) { if ((*p & 0xc0) != 0x80) return item->id; code = (code << 6) | (*p++ & 0x3f); }
        lv_font_glyph_dsc_t glyph;
        if (!music_font_18.get_glyph_dsc(&music_font_18, &glyph, code, 0)) return item->id;
    }
    return item->title;
}

static void release_remote(void)
{
    free(s_remote_owner); s_remote_owner = NULL; s_remote = NULL; s_remote_count = 0;
}

static bool cache_selected(const music_item_t *item)
{
    for (int i = 0; i < s_selected_count; ++i)
        if (!strcmp(s_selected[i].id, item->id)) return true;
    return false;
}

static int menu_item_count(void)
{
    switch (s_page) {
    case PAGE_MENU: return 4;
    case PAGE_SETTINGS: return 5;
    case PAGE_SCAN: return (s_wifi_count > 0 ? s_wifi_count : 0) + 2;
    case PAGE_AUTH: return 3;
    case PAGE_KEYBOARD: return (int)strlen(s_key_groups[s_key_group]) + 4;
    case PAGE_LOCAL: return s_local_count + 2;
    case PAGE_ONLINE: case PAGE_CACHE: return s_remote_count + 5;
    case PAGE_CONFIRM: case PAGE_BUSY: return 2;
    case PAGE_VOLUME: return 2;
    case PAGE_WIFI:
#if CONFIG_MUSIC_ONLINE_P0
        return music_network_state() == MUSIC_NET_PROVISIONING ? 6 : 4;
#else
        return 4;
#endif
    default: return 1;
    }
}

static void menu_text(int item, char *text, size_t size)
{
    const char *value = "返回";
    const char *home[] = {"在线播放", "本地播放", "缓存歌曲", "系统设置"};
    const char *settings[] = {"音量调节", "Wi-Fi 连接", "播放模式", "音乐库账号", "返回"};
    const char *navigation[] = {"上一页", "下一页", "刷新曲库", "返回"};
    switch (s_page) {
    case PAGE_MENU: value = home[item]; break;
    case PAGE_SETTINGS: value = settings[item]; break;
    case PAGE_SCAN:
        value = item < s_wifi_count ? s_wifi_names[item] :
            item == (s_wifi_count > 0 ? s_wifi_count : 0) ? "手动输入网络名" : "返回";
        break;
    case PAGE_AUTH:
        value = item == 0 ? "修改用户名" : item == 1 ? "输入密码并保存" : "返回";
        break;
    case PAGE_LOCAL:
        value = item == 0 ? "随机播放" : (item <= s_local_count ? display_title(&s_local[item - 1]) : "返回"); break;
    case PAGE_ONLINE:
        value = item == 0 ? "随机播放" : (item <= s_remote_count ? display_title(&s_remote[item - 1]) : navigation[item - s_remote_count - 1]); break;
    case PAGE_CACHE:
        if (item < s_remote_count) {
            snprintf(text, size, "[%c] %s", cache_selected(&s_remote[item]) ? 'x' : ' ', display_title(&s_remote[item])); return;
        }
        if (item == s_remote_count + 2) { snprintf(text, size, "应用更改 (%d/5)", s_selected_count); return; }
        value = item == s_remote_count ? "上一页" : item == s_remote_count + 1 ? "下一页" : item == s_remote_count + 3 ? "刷新曲库" : "返回";
        break;
    case PAGE_CONFIRM: value = item == 0 ? "确认下载或移除" : "取消"; break;
    case PAGE_BUSY: value = item == 0 ? "正在处理" : "取消并返回"; break;
    case PAGE_VOLUME:
        if (item == 0) { snprintf(text, size, "音量 %u/10", (unsigned)s_volume_edit); return; }
        value = "确定保存 / 长按取消"; break;
    case PAGE_WIFI:
#if CONFIG_MUSIC_ONLINE_P0
        if (music_network_state() == MUSIC_NET_PROVISIONING) {
            char name[33], password[9]; music_network_ap_info(name, sizeof(name), password, sizeof(password));
            if (item == 0) snprintf(text, size, "AP: %s", name);
            else if (item == 1) snprintf(text, size, "密码: %s", password);
            else snprintf(text, size, "%s", item == 2 ? "http://192.168.4.1" : item == 3 ? "5 分钟后关闭" : item == 4 ? "结束配网" : "返回");
            return;
        }
        if (item == 0) {
            const char *states[] = {"未连接", "连接中", "已连接 / 正在校时", "联网就绪", "配网中", "连接失败", "校时超时 正在重试"};
            value = states[music_network_state()];
            if (music_network_state() == MUSIC_NET_ERROR)
                value = music_network_disconnect_reason() == -2 ? "校时超时 请重连" : "连接失败 请检查密码";
        } else value = item == 1 ? "重新连接" : item == 2 ? "选择 Wi-Fi" : "返回";
#else
        value = item == 0 ? "联网功能未启用" : item == 1 ? "重新连接" : item == 2 ? "手机配网" : "返回";
#endif
        break;
    default: break;
    }
    snprintf(text, size, "%s", value);
}

static void menu_refresh_locked(void)
{
    const int count = menu_item_count();
    if (s_page == PAGE_KEYBOARD) {
        size_t length = strlen(s_edit_text);
        char shown[140];
        if (s_edit_field == 1 || s_edit_field == 3) {
            memset(shown, '*', length); shown[length] = 0;
            if (length && esp_timer_get_time() < s_reveal_until) shown[length - 1] = s_edit_text[length - 1];
        } else snprintf(shown, sizeof(shown), "%s", s_edit_text);
        lv_label_set_text(s_key_text, shown);
        for (int i = 0; i < count; ++i)
            lv_obj_set_style_bg_color(s_key_labels[i], lv_color_hex(i == s_menu_selected ? COLOR_SELECTED : COLOR_PANEL), 0);
        return;
    }
    if (s_menu_selected >= count) s_menu_selected = count - 1;
    if (s_menu_selected < s_menu_top) s_menu_top = s_menu_selected;
    if (s_menu_selected >= s_menu_top + MENU_VISIBLE_ROWS) s_menu_top = s_menu_selected - MENU_VISIBLE_ROWS + 1;
    if (s_menu_top < 0) s_menu_top = 0;
    for (int row = 0; row < MENU_VISIBLE_ROWS; ++row) {
        int item = s_menu_top + row;
        if (item >= count) { lv_obj_add_flag(s_menu_rows[row], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(s_menu_rows[row], LV_OBJ_FLAG_HIDDEN);
        bool selected = item == s_menu_selected;
        char text[160]; menu_text(item, text, sizeof(text));
        if (strcmp(lv_label_get_text(s_menu_labels[row]), text))
            lv_label_set_text(s_menu_labels[row], text);
        lv_label_long_mode_t mode = selected ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_CLIP;
        if (lv_label_get_long_mode(s_menu_labels[row]) != mode)
            lv_label_set_long_mode(s_menu_labels[row], mode);
        lv_obj_set_style_bg_color(s_menu_rows[row], lv_color_hex(selected ? COLOR_SELECTED : COLOR_PANEL), 0);
    }
}

static void build_list_locked(page_t page)
{
    destroy_screen_locked(); s_page = page; s_menu_selected = s_menu_top = 0;
    if (page == PAGE_KEYBOARD) {
        const char *titles[] = {"输入网络名", "输入网络密码", "输入曲库用户名", "输入曲库密码"};
        s_screen = make_screen(titles[s_edit_field]);
        s_key_text = make_label(s_screen, "", lv_color_hex(COLOR_TEXT));
        lv_obj_set_pos(s_key_text, 8, 44); lv_obj_set_size(s_key_text, 224, 32);
        lv_label_set_long_mode(s_key_text, LV_LABEL_LONG_SCROLL);
        int letters = strlen(s_key_groups[s_key_group]);
        const char *actions[] = {"组", "删", "好", "退"};
        for (int i = 0; i < letters + 4; ++i) {
            char character[2] = {i < letters ? s_key_groups[s_key_group][i] : 0, 0};
            s_key_labels[i] = make_label(s_screen, i < letters ? (character[0] == ' ' ? "空" : character) : actions[i - letters], lv_color_hex(COLOR_TEXT));
            lv_obj_set_pos(s_key_labels[i], (i % 8) * 30, 82 + (i / 8) * 38);
            lv_obj_set_size(s_key_labels[i], 29, 37);
            lv_obj_set_style_text_align(s_key_labels[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_bg_opa(s_key_labels[i], LV_OPA_COVER, 0);
        }
        make_footer(s_screen, s_notice[0] ? s_notice : "组换字符 删删除 好完成 退取消");
        menu_refresh_locked(); lv_screen_load(s_screen); return;
    }
    const char *titles[] = {"儿童音乐播放器", "本地播放", "在线播放", "缓存歌曲", "系统设置", "音量调节", "Wi-Fi 连接", "确认更改", "缓存处理中", "选择网络", "音乐库账号"};
    s_screen = make_screen(titles[page]);
    for (int row = 0; row < MENU_VISIBLE_ROWS; ++row)
        s_menu_rows[row] = make_row(s_screen, 45 + row * 40, "", false, &s_menu_labels[row]);
    make_footer(s_screen, s_notice[0] ? s_notice : "上下选择  确定进入");
    menu_refresh_locked(); lv_screen_load(s_screen);
}

static void build_menu_locked(void) { s_notice[0] = 0; build_list_locked(PAGE_MENU); }

static void mode_refresh_locked(void)
{
    for (int i = 0; i <= MUSIC_MODE_COUNT; i++) {
        const bool selected = i == (int)s_mode_selection;
        lv_label_set_text(s_mode_labels[i], i == MUSIC_MODE_COUNT ? "返回" : s_mode_names[i]);
        lv_label_set_long_mode(s_mode_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(s_mode_rows[i],
                                  selected ? lv_color_hex(COLOR_SELECTED)
                                           : lv_color_hex(COLOR_PANEL), 0);
    }
}

static void build_mode_page_locked(void)
{
    destroy_screen_locked();
    s_page = PAGE_MODE;
    s_mode_selection = s_mode;
    s_screen = make_screen("播放模式");
    for (int i = 0; i <= MUSIC_MODE_COUNT; i++) {
        s_mode_rows[i] = make_row(s_screen, 65 + i * 42, i == MUSIC_MODE_COUNT ? "返回" : s_mode_names[i],
                                  i == (int)s_mode_selection, &s_mode_labels[i]);
    }
    make_footer(s_screen, "上下选择    确定保存");
    mode_refresh_locked();
    lv_screen_load(s_screen);
}

static void playback_refresh_locked(void)
{
    if (!s_play_title || !s_play_status || !s_play_volume || !s_play_mode ||
        s_current_song < 0 || s_current_song >= song_count()) {
        return;
    }
    lv_label_set_text(s_play_title, display_title(&s_current_item));
    const char *status = s_loading ? "连接中" : s_paused ? "已暂停" : "播放中";
    if (!s_playing) {
        status = is_online_song(s_current_song)
            ? (s_play_error == ESP_ERR_INVALID_RESPONSE ? "Restart required" : "Error")
            : "音频未安装";
    }
    lv_label_set_text(s_play_status, status);

    char volume[32];
    snprintf(volume, sizeof(volume), "音量 %u/10", (unsigned)s_volume);
    lv_label_set_text(s_play_volume, volume);

    char mode[32];
    snprintf(mode, sizeof(mode), "模式：%s", s_mode_names[s_mode]);
    lv_label_set_text(s_play_mode, mode);
}

static void build_playback_page_locked(void)
{
    destroy_screen_locked();
    s_page = PAGE_PLAYBACK;
    s_screen = make_screen("正在播放");

    s_play_title = make_label(s_screen, "", lv_color_hex(COLOR_TEXT));
    lv_obj_set_pos(s_play_title, 10, 92);
    lv_obj_set_size(s_play_title, 220, 64);
    lv_label_set_long_mode(s_play_title, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(s_play_title, LV_TEXT_ALIGN_CENTER, 0);
    /* Long titles scroll at the enlarged accessibility font size. */

    s_play_status = make_label(s_screen, "", lv_color_hex(COLOR_ACCENT));
    lv_obj_set_pos(s_play_status, 10, 164);
    lv_obj_set_size(s_play_status, 220, 34);
    lv_obj_set_style_text_align(s_play_status, LV_TEXT_ALIGN_CENTER, 0);

    s_play_volume = make_label(s_screen, "", lv_color_hex(COLOR_MUTED));
    lv_obj_set_pos(s_play_volume, 10, 204);
    lv_obj_set_size(s_play_volume, 220, 34);
    lv_obj_set_style_text_align(s_play_volume, LV_TEXT_ALIGN_CENTER, 0);

    s_play_mode = make_label(s_screen, "", lv_color_hex(COLOR_MUTED));
    lv_obj_set_pos(s_play_mode, 10, 236);
    lv_obj_set_size(s_play_mode, 220, 34);
    lv_obj_set_style_text_align(s_play_mode, LV_TEXT_ALIGN_CENTER, 0);

    make_footer(s_screen, "上下切歌  确定暂停");
    playback_refresh_locked();
    lv_screen_load(s_screen);
}

/* -------------------------------------------------------------------------- */
/* Playback state and five-second display timeout                             */
/* -------------------------------------------------------------------------- */

static void blank_timer_cancel(void)
{
    if (s_blank_timer) {
        (void)esp_timer_stop(s_blank_timer);
    }
}

static void blank_timer_start(void)
{
    if (!s_blank_timer || !s_screen_awake || !s_playing || s_paused) {
        return;
    }
    blank_timer_cancel();
    s_blank_cookie++;
    (void)esp_timer_start_once(s_blank_timer, AUTO_BLANK_US);
}

static void start_song_locked(int song, bool wake_screen)
{
    if (song < 0 || song >= song_count()) {
        return;
    }

    s_play_error = ESP_OK;
    s_play_generation++;
    s_current_song = song;
    s_playing = true;
    s_loading = is_online_song(song);
    if (s_loading) {
        release_remote();
        memset(&s_current_item, 0, sizeof(s_current_item));
        s_current_item.online = true;
        snprintf(s_current_item.title, sizeof(s_current_item.title), "连接中");
        (void)send_player_command(PLAYER_CMD_STOP, -1, s_volume, s_play_generation);
        if (!music_library_track(song - MUSIC_LOCAL_MAX, s_release, ++s_library_generation)) {
            s_loading = false; s_playing = false; s_play_error = ESP_FAIL;
        }
    } else {
        if (song >= s_local_count) { s_playing = false; return; }
        s_current_item = s_local[song];
    }
    if (wake_screen) {
        s_screen_awake = true;
        bsp_display_backlight(100);
    }
    if (!is_online_song(song) && !send_player_command(PLAYER_CMD_START, song, s_volume, s_play_generation)) s_playing = false;
    if (s_page != PAGE_PLAYBACK) {
        build_playback_page_locked();
    } else {
        playback_refresh_locked();
    }
    blank_timer_start();
}

static void shuffle_for_song(int first_song)
{
    int count = s_play_online ? s_remote_total : s_local_count;
    for (int i = 0; i < count; ++i) s_shuffle[i] = (uint16_t)i;
    for (int i = count - 1; i > 0; --i) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint16_t swap = s_shuffle[i]; s_shuffle[i] = s_shuffle[j]; s_shuffle[j] = swap;
    }
    int first = first_song - (s_play_online ? MUSIC_LOCAL_MAX : 0);
    if (first >= 0 && first < count) {
        for (int i = 0; i < count; ++i) if (s_shuffle[i] == first) {
            uint16_t swap = s_shuffle[0]; s_shuffle[0] = s_shuffle[i]; s_shuffle[i] = swap; break;
        }
    }
    s_shuffle_position = 0;
}

static int step_song_locked(int direction, bool automatic)
{
    int count = s_play_online ? s_remote_total : s_local_count;
    int offset = s_play_online ? MUSIC_LOCAL_MAX : 0;
    if (count == 0) return -1;
    if (automatic && s_mode == MUSIC_MODE_SINGLE) return s_current_song;
    if (s_mode != MUSIC_MODE_RANDOM) return offset + (s_current_song - offset + direction + count) % count;
    if (s_shuffle_position < 0) shuffle_for_song(s_current_song);
    int next = music_playlist_step(s_shuffle, count, s_shuffle_position, direction);
    s_shuffle_position = (s_shuffle_position + direction + count) % count;
    /* A bounded compact permutation, not full metadata for the whole library.
     * Previous/next walk this round's history; next at the boundary starts a new round. */
    if (direction == 1 && s_shuffle_position == 0) {
        shuffle_for_song(-1); next = s_shuffle[0];
        if (count > 1 && next + offset == s_current_song) {
            uint16_t swap = s_shuffle[0]; s_shuffle[0] = s_shuffle[1]; s_shuffle[1] = swap; next = s_shuffle[0];
        }
    }
    return offset + next;
}

static int next_song_locked(void) { return step_song_locked(1, true); }

static void stop_to_menu_locked(void)
{
    s_play_generation++;
    s_playing = false;
    s_paused = false;
    s_loading = false;
    music_library_cancel(); ++s_library_generation;
    s_current_song = -1;
    s_screen_awake = true;
    s_ignore_ok_until_us = esp_timer_get_time() + 500000;
    blank_timer_cancel();
    bsp_display_backlight(100);
    (void)send_player_command(PLAYER_CMD_STOP, -1, s_volume, s_play_generation);
    s_notice[0] = 0;
    if (s_play_online) fetch_page_locked(PAGE_ONLINE, s_remote_page, false);
    else build_list_locked(PAGE_LOCAL);
}

static void handle_volume_locked(int delta)
{
    int next = (int)s_volume_edit + delta;
    if (next < VOLUME_MIN) {
        next = VOLUME_MIN;
    }
    if (next > VOLUME_MAX) {
        next = VOLUME_MAX;
    }
    s_volume_edit = (uint8_t)next;
    menu_refresh_locked();
}

/* -------------------------------------------------------------------------- */
/* Input event handling                                                       */
/* -------------------------------------------------------------------------- */

static void notice_locked(page_t page, const char *message)
{
    snprintf(s_notice, sizeof(s_notice), "%s", message);
    build_list_locked(page);
}

static void fetch_page_locked(page_t page, int number, bool refresh)
{
    music_library_cancel();
    s_loading = true; release_remote();
    snprintf(s_notice, sizeof(s_notice), "读取曲库中");
    build_list_locked(page);
    if (!music_library_page(number, refresh, ++s_library_generation)) {
        s_loading = false; notice_locked(page, "曲库任务不可用");
    }
}

static void handle_button_locked(bsp_btn_t button, bsp_btn_ev_t event)
{
    // Some button-component versions may report a trailing click after LONG.
    // Ignore only that short release window; do not make the next real menu
    // selection disappear when a long press did not generate a click.
    if (button == BSP_BTN_OK && event == BSP_BTN_CLICK &&
        esp_timer_get_time() < s_ignore_ok_until_us) {
        return;
    }

    if (s_page == PAGE_KEYBOARD) {
        int letters = strlen(s_key_groups[s_key_group]);
        if (button == BSP_BTN_OK && event == BSP_BTN_LONG) {
            memset(s_edit_text, 0, sizeof(s_edit_text));
            s_ignore_ok_until_us = esp_timer_get_time() + 500000;
            build_list_locked(s_edit_field >= 2 ? PAGE_AUTH : PAGE_WIFI); return;
        }
        if (event != BSP_BTN_CLICK) return;
        if (button != BSP_BTN_OK) {
            s_menu_selected = (s_menu_selected + (button == BSP_BTN_UP ? letters + 3 : 1)) % (letters + 4);
            menu_refresh_locked(); return;
        }
        size_t length = strlen(s_edit_text);
        size_t maximum = s_edit_field == 0 ? 32 : s_edit_field == 1 ? 63 : s_edit_field == 2 ? 64 : 128;
        if (s_menu_selected < letters) {
            if (length < maximum) { s_edit_text[length] = s_key_groups[s_key_group][s_menu_selected]; s_edit_text[length + 1] = 0; s_reveal_until = esp_timer_get_time() + 1000000; }
            menu_refresh_locked(); return;
        }
        int action = s_menu_selected - letters;
        if (action == 0) { s_key_group = (s_key_group + 1) % 4; build_list_locked(PAGE_KEYBOARD); }
        else if (action == 1) { if (length) s_edit_text[length - 1] = 0; menu_refresh_locked(); }
        else if (action == 3) { memset(s_edit_text, 0, sizeof(s_edit_text)); build_list_locked(s_edit_field >= 2 ? PAGE_AUTH : PAGE_WIFI); }
        else if (s_edit_field == 0) {
            if (!length) return;
            snprintf(s_wifi_name, sizeof(s_wifi_name), "%.32s", s_edit_text); begin_edit(1);
        } else if (s_edit_field == 2) {
            if (strchr(s_edit_text, ':')) return;
            snprintf(s_account_name, sizeof(s_account_name), "%.64s", s_edit_text);
            memset(s_edit_text, 0, sizeof(s_edit_text)); build_list_locked(PAGE_AUTH);
        } else {
#if CONFIG_MUSIC_ONLINE_P0
            bool ok = s_edit_field == 1 ? music_network_save_wifi(s_wifi_name, s_edit_text) : music_network_save_auth(s_account_name, s_edit_text);
            if (!ok) { strcpy(s_notice, "输入无效或任务忙"); build_list_locked(PAGE_KEYBOARD); return; }
            memset(s_edit_text, 0, sizeof(s_edit_text));
            notice_locked(s_edit_field == 1 ? PAGE_WIFI : PAGE_AUTH, "正在保存");
#else
            notice_locked(PAGE_SETTINGS, "联网功能未启用");
#endif
        }
        return;
    }
    if (s_page < PAGE_MODE) {
        if (button == BSP_BTN_OK && event == BSP_BTN_LONG && s_page != PAGE_MENU) {
            s_ignore_ok_until_us = esp_timer_get_time() + 500000;
            bool cache_busy = s_page == PAGE_BUSY;
            music_library_cancel(); ++s_library_generation; s_loading = false; s_pending_action = 0;
            if (cache_busy) (void)music_library_local(s_library_generation);
#if CONFIG_MUSIC_ONLINE_P0
            if (s_page == PAGE_WIFI) music_network_cancel_provision();
#endif
            if (s_page == PAGE_VOLUME) { s_notice[0] = 0; build_list_locked(PAGE_SETTINGS); }
            else build_menu_locked();
            return;
        }
        if (event != BSP_BTN_CLICK) return;
        if (s_page == PAGE_VOLUME) {
            if (button == BSP_BTN_UP) handle_volume_locked(1);
            else if (button == BSP_BTN_DOWN) handle_volume_locked(-1);
            else if (button == BSP_BTN_OK) {
                s_volume = s_volume_edit; post_settings(true);
                (void)send_player_command(PLAYER_CMD_SET_VOLUME, -1, s_volume, s_play_generation);
                s_notice[0] = 0; build_list_locked(PAGE_SETTINGS);
            }
            return;
        }
        int count = menu_item_count();
        if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
            s_menu_selected = (s_menu_selected + (button == BSP_BTN_UP ? count - 1 : 1)) % count;
            menu_refresh_locked(); return;
        }
        if (button != BSP_BTN_OK) return;
        int selected = s_menu_selected;
        page_t page = s_page;
        if (page == PAGE_MENU) {
            s_notice[0] = 0;
            if (selected == 0 || selected == 2) fetch_page_locked(selected == 0 ? PAGE_ONLINE : PAGE_CACHE, 0, false);
            else build_list_locked(selected == 1 ? PAGE_LOCAL : PAGE_SETTINGS);
        } else if (page == PAGE_SETTINGS) {
            s_notice[0] = 0;
            if (selected == 0) { s_volume_edit = s_volume; build_list_locked(PAGE_VOLUME); }
            else if (selected == 1) build_list_locked(PAGE_WIFI);
            else if (selected == 2) build_mode_page_locked();
            else if (selected == 3) build_list_locked(PAGE_AUTH);
            else build_menu_locked();
        } else if (page == PAGE_SCAN) {
            if (selected < s_wifi_count) { strcpy(s_wifi_name, s_wifi_names[selected]); begin_edit(1); }
            else if (selected == (s_wifi_count > 0 ? s_wifi_count : 0)) begin_edit(0);
            else build_list_locked(PAGE_WIFI);
        } else if (page == PAGE_AUTH) {
            if (selected < 2) begin_edit(selected == 0 ? 2 : 3);
            else build_list_locked(PAGE_SETTINGS);
        } else if (page == PAGE_LOCAL || page == PAGE_ONLINE) {
            int tracks = page == PAGE_LOCAL ? s_local_count : s_remote_count;
            if (selected <= tracks) {
                int total = page == PAGE_LOCAL ? s_local_count : s_remote_total;
                if (s_loading || total == 0) { notice_locked(page, s_loading ? "读取曲库中" : "没有可播放歌曲"); return; }
                s_play_online = page == PAGE_ONLINE; s_paused = false;
                int offset = s_play_online ? MUSIC_LOCAL_MAX : 0;
                int song = selected ? (s_play_online ? s_remote[selected - 1].index : selected - 1) + offset : offset;
                if (selected == 0) { s_mode = MUSIC_MODE_RANDOM; post_settings(true); shuffle_for_song(-1); song = offset + s_shuffle[0]; }
                else if (s_mode == MUSIC_MODE_RANDOM) shuffle_for_song(song);
                else s_shuffle_position = -1;
                start_song_locked(song, true);
            } else if (page == PAGE_LOCAL || selected == tracks + 4) {
                music_library_cancel(); ++s_library_generation; s_loading = false; build_menu_locked();
            } else if (selected == tracks + 3) fetch_page_locked(page, 0, true);
            else if (!s_loading) {
                int target = s_remote_page + (selected == tracks + 1 ? -1 : 1);
                if (target >= 0 && target * MUSIC_PAGE_SIZE < s_remote_total) fetch_page_locked(page, target, false);
            }
        } else if (page == PAGE_CACHE) {
            if (selected < s_remote_count) {
                if (!music_cache_toggle(s_selected, &s_selected_count, &s_remote[selected])) notice_locked(page, "最多选择五首");
                else menu_refresh_locked();
            } else if (selected == s_remote_count + 4) {
                music_library_cancel(); ++s_library_generation; s_loading = false; build_menu_locked();
            } else if (selected == s_remote_count + 2) {
                if (!s_loading) notice_locked(PAGE_CONFIRM, "先移除未选歌曲，再下载");
            } else if (selected == s_remote_count + 3) fetch_page_locked(page, 0, true);
            else if (!s_loading) {
                int target = s_remote_page + (selected == s_remote_count ? -1 : 1);
                if (target >= 0 && target * MUSIC_PAGE_SIZE < s_remote_total) fetch_page_locked(page, target, false);
            }
        } else if (page == PAGE_CONFIRM) {
            if (selected == 1) { s_notice[0] = 0; build_list_locked(PAGE_CACHE); }
            else {
                s_pending_action = 1; ++s_play_generation;
                if (!send_player_command(PLAYER_CMD_STOP, -1, s_volume, s_play_generation)) { s_pending_action = 0; notice_locked(PAGE_CACHE, "播放器任务忙"); }
                else { release_remote(); notice_locked(PAGE_BUSY, "停止播放后处理"); }
            }
        } else if (page == PAGE_BUSY) {
            if (selected == 1) { music_library_cancel(); ++s_library_generation; s_pending_action = 0; s_notice[0] = 0; build_list_locked(PAGE_CACHE); (void)music_library_local(s_library_generation); }
        } else if (page == PAGE_WIFI) {
#if CONFIG_MUSIC_ONLINE_P0
            if (music_network_state() == MUSIC_NET_PROVISIONING) {
                if (selected >= 4) { music_network_cancel_provision(); s_notice[0] = 0; build_list_locked(PAGE_SETTINGS); }
            } else if (selected == 1) {
                music_network_reconnect(); notice_locked(PAGE_WIFI, "正在重新连接");
            } else if (selected == 2) {
                music_library_cancel(); ++s_library_generation;
                s_wifi_count = -1;
                if (music_network_scan()) notice_locked(PAGE_SCAN, "正在扫描网络");
                else notice_locked(PAGE_WIFI, "网络任务忙");
            } else if (selected == 3) { s_notice[0] = 0; build_list_locked(PAGE_SETTINGS); }
#else
            if (selected == 3) { s_notice[0] = 0; build_list_locked(PAGE_SETTINGS); }
            else notice_locked(PAGE_WIFI, "联网功能未启用");
#endif
        }
        return;
    }

    if (s_page == PAGE_MODE) {
        if (button == BSP_BTN_OK && event == BSP_BTN_LONG) { s_ignore_ok_until_us = esp_timer_get_time() + 500000; s_notice[0] = 0; build_list_locked(PAGE_SETTINGS); return; }
        if (event != BSP_BTN_CLICK) return;
        if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
            s_mode_selection = (music_mode_t)((s_mode_selection + (button == BSP_BTN_UP ? MUSIC_MODE_COUNT : 1)) % (MUSIC_MODE_COUNT + 1));
            mode_refresh_locked();
        } else if (button == BSP_BTN_OK) {
            if (s_mode_selection < MUSIC_MODE_COUNT) { s_mode = s_mode_selection; s_shuffle_position = -1; post_settings(true); }
            s_notice[0] = 0; build_list_locked(PAGE_SETTINGS);
        }
        return;
    }

    /* Playback page: LONG wakes a blank display; on a lit display it returns
     * to the song list and stops the current song. */
    if (button == BSP_BTN_OK && event == BSP_BTN_LONG) {
        if (!s_screen_awake) {
            // Some button-component versions may emit a trailing CLICK on
            // release. Suppress it so wake-up cannot immediately pause.
            s_ignore_ok_until_us = esp_timer_get_time() + 500000;
            s_screen_awake = true;
            bsp_display_backlight(100);
            if (s_page != PAGE_PLAYBACK) {
                build_playback_page_locked();
            } else {
                playback_refresh_locked();
            }
            blank_timer_start();
        } else {
            stop_to_menu_locked();
        }
        return;
    }
    if ((button == BSP_BTN_UP || button == BSP_BTN_DOWN) && event == BSP_BTN_CLICK) {
        int next = step_song_locked(button == BSP_BTN_UP ? -1 : 1, false);
        if (next >= 0) start_song_locked(next, false);
    } else if (button == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        // A short OK press while blank changes only the audio state.  The
        // display remains off; a LONG press is the explicit wake-up action.
        if (s_paused) {
            s_paused = false;
            (void)send_player_command(PLAYER_CMD_RESUME, -1, s_volume,
                                      s_play_generation);
            playback_refresh_locked();
            blank_timer_start();
        } else if (s_playing) {
            s_paused = true;
            blank_timer_cancel();
            (void)send_player_command(PLAYER_CMD_PAUSE, -1, s_volume,
                                      s_play_generation);
            playback_refresh_locked();
        }
    }
}

static void sync_selection(void)
{
    s_selected_count = 0;
    for (int i = 0; i < s_local_count && s_selected_count < MUSIC_CACHE_MAX; ++i) {
        if (s_local[i].sha256[0]) {
            music_item_t *item = &s_selected[s_selected_count++];
            *item = s_local[i]; item->online = true;
        }
    }
}

static void handle_library_locked(music_library_event_t *event)
{
    if (!event || event->generation != s_library_generation) {
        ESP_LOGW(TAG, "lib event stale/dropped kind=%d gen=%u expected=%u",
                 event ? (int)event->kind : -1, event ? event->generation : 0, s_library_generation);
        return;
    }
    if (event->kind == MUSIC_LIBRARY_PAGE && (s_page == PAGE_ONLINE || s_page == PAGE_CACHE)) {
        s_loading = false; s_remote_count = event->count; s_remote_total = event->total; s_remote_page = event->page;
        strcpy(s_release, event->release);
        free(s_remote_owner); s_remote_owner = event; s_remote = event->items;
        snprintf(s_notice, sizeof(s_notice), "第 %d 页 / %d 首", event->page + 1, event->total);
        build_list_locked(s_page);
    } else if (event->kind == MUSIC_LIBRARY_TRACK && s_page == PAGE_PLAYBACK) {
        s_loading = false; s_current_item = event->items[0];
        if (!send_player_command(PLAYER_CMD_START, s_current_song, s_volume, s_play_generation)) { s_playing = false; s_play_error = ESP_FAIL; }
        playback_refresh_locked(); blank_timer_start();
    } else if (event->kind == MUSIC_LIBRARY_LOCAL) {
        s_local_count = event->count; memcpy(s_local, event->items, (size_t)event->count * sizeof(*s_local)); sync_selection();
        if (s_page == PAGE_CACHE && !s_remote && !s_loading) fetch_page_locked(PAGE_CACHE, 0, false);
        else if (s_page == PAGE_LOCAL || s_page == PAGE_CACHE) menu_refresh_locked();
    } else if (event->kind == MUSIC_LIBRARY_PROGRESS && s_page == PAGE_BUSY) {
        snprintf(s_notice, sizeof(s_notice), "下载进度 %d%%", event->progress);
        build_list_locked(PAGE_BUSY);
    } else if (event->kind == MUSIC_LIBRARY_DONE && s_page == PAGE_BUSY) {
        notice_locked(PAGE_CACHE, "缓存更改已保存");
        (void)music_library_local(++s_library_generation);
    } else if (event->kind == MUSIC_LIBRARY_ERROR) {
        s_loading = false;
        if (s_page == PAGE_PLAYBACK) {
            s_playing = s_paused = false; s_play_error = event->error; blank_timer_cancel();
            s_screen_awake = true; bsp_display_backlight(100); playback_refresh_locked();
        } else {
            const char *message = event->error == ESP_ERR_NOT_ALLOWED ? "音乐库用户名或密码错误" :
                event->error == ESP_ERR_HTTP_CONNECT ? "无法连接曲库服务器" :
                event->error == MUSIC_ERR_STORAGE_FULL ? "歌曲存储空间不足" :
                event->error == ESP_ERR_NO_MEM ? "运行内存不足" :
                event->error == ESP_ERR_INVALID_STATE ? "未联网或版本变化" :
                event->error == ESP_ERR_NVS_NOT_FOUND || event->error == ESP_ERR_INVALID_ARG ? "请配置 Wi-Fi 和音乐库账号" :
                event->error == ESP_ERR_NOT_SUPPORTED ? "联网功能未启用" : "曲库请求失败";
            bool cache_busy = s_page == PAGE_BUSY;
            notice_locked(cache_busy ? PAGE_CACHE : s_page, message);
            if (cache_busy) (void)music_library_local(++s_library_generation);
        }
    }
}

static void handle_app_event_locked(const app_event_t *event)
{
    if (!event) {
        return;
    }
    switch (event->type) {
    case APP_EVENT_BUTTON:
        handle_button_locked(event->button, event->button_event);
        break;
    case APP_EVENT_LIBRARY:
        handle_library_locked(event->library);
        if (event->library != s_remote_owner) free(event->library);
        break;
    case APP_EVENT_PLAYER_STOPPED:
        if (event->generation == s_play_generation && s_pending_action) {
            int action = s_pending_action; s_pending_action = 0;
            if (action == 1 && s_page == PAGE_BUSY) {
                if (!music_library_apply(s_selected, s_selected_count, ++s_library_generation)) notice_locked(PAGE_CACHE, "缓存任务不可用");
            }
#if CONFIG_MUSIC_ONLINE_P0
            else if (action == 2 && s_page == PAGE_WIFI) {
                if (!music_network_provision()) notice_locked(PAGE_WIFI, "网络任务不可用");
            }
#endif
        }
        break;
    case APP_EVENT_TEST_REPORT:
#if CONFIG_MUSIC_TEST_CONSOLE
        ESP_LOGI(TAG, "TEST page=%d selected=%d volume=%u mode=%d song=%d playing=%d paused=%d loading=%d heap=%zu largest=%zu audio_stack=%u awake=%d local=%d",
                 s_page, s_menu_selected, s_volume, s_mode, s_current_song, s_playing, s_paused, s_loading,
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(s_player_task), s_screen_awake, s_local_count);
#if CONFIG_MUSIC_ONLINE_P0
        ESP_LOGI(TAG, "TEST network=%d reason=%d scan_count=%d save_status=%d", music_network_state(), music_network_disconnect_reason(), s_wifi_count, music_network_save_status());
#endif
#endif
        break;
    case APP_EVENT_BLANK:
        if (s_page == PAGE_PLAYBACK && s_playing && !s_paused &&
            s_screen_awake && event->blank_cookie == s_blank_cookie) {
            s_screen_awake = false;
            bsp_display_backlight(0);
        }
        break;
    case APP_EVENT_PLAYER_FINISHED:
        if (s_page == PAGE_PLAYBACK && s_playing &&
            event->generation == s_play_generation) {
            const int next = next_song_locked();
            start_song_locked(next, false);
        }
        break;
    case APP_EVENT_PLAYER_ERROR:
        if (s_page == PAGE_PLAYBACK && event->generation == s_play_generation) {
            s_play_error = event->error;
            s_playing = false;
            s_paused = false;
            s_loading = false;
            blank_timer_cancel();
            s_screen_awake = true;
            bsp_display_backlight(100);
            playback_refresh_locked();
            ESP_LOGE(TAG, "歌曲 %d 播放失败: %d", event->song, event->error);
        }
        break;
    default:
        break;
    }
}

static void input_worker(void *argument)
{
    (void)argument;
    app_event_t event;
    for (;;) {
        bool received = xQueueReceive(s_app_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE;
        if (!bsp_lvgl_lock(1000)) {
            if (received && event.type == APP_EVENT_LIBRARY) free(event.library);
            ESP_LOGW(TAG, "获取 LVGL 锁超时，丢弃应用事件");
            continue;
        }
        if (received) handle_app_event_locked(&event);
        if (s_page == PAGE_WIFI) menu_refresh_locked();
        if (s_page == PAGE_KEYBOARD && s_reveal_until && esp_timer_get_time() >= s_reveal_until) {
            s_reveal_until = 0; menu_refresh_locked();
        }
#if CONFIG_MUSIC_ONLINE_P0
        if (s_page == PAGE_SCAN) {
            int count = music_network_scan_results(s_wifi_names);
            if (count != s_wifi_count) {
                s_wifi_count = count;
                notice_locked(PAGE_SCAN, count == -2 ? "扫描失败 可手动输入" : count == 0 ? "没有发现网络" : "选择网络并输入密码");
            }
        }
        if ((s_page == PAGE_AUTH || s_page == PAGE_WIFI) && !strcmp(s_notice, "正在保存")) {
            int status = music_network_save_status();
            if (status == 2 || status == -1) notice_locked(s_page, status == 2 ? "已保存" : "保存失败 请重试");
        }
#endif
        bsp_lvgl_unlock();
    }
}

static void blank_timer_callback(void *argument)
{
    (void)argument;
    app_event_t event = {
        .type = APP_EVENT_BLANK,
        .blank_cookie = s_blank_cookie,
    };
    post_app_event(&event);
}

/* -------------------------------------------------------------------------- */
/* Public entry points                                                        */
/* -------------------------------------------------------------------------- */

void music_app_on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (!s_started || !s_app_queue) {
        return;
    }
    const app_event_t app_event = {
        .type = APP_EVENT_BUTTON,
        .button = button,
        .button_event = event,
    };
    // This function runs in the button component's timer task: no LVGL, no
    // I2S, no filesystem, no NVS and no blocking operation here.
    (void)xQueueSend(s_app_queue, &app_event, 0);
}

#if CONFIG_MUSIC_TEST_CONSOLE
static void test_console(void *argument)
{
    (void)argument;
    setvbuf(stdin, NULL, _IONBF, 0);
    for (;;) {
        int c = getchar();
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == 's') {
            const app_event_t event = {.type = APP_EVENT_TEST_REPORT}; post_app_event(&event);
        } else if (c == 'u' || c == 'd' || c == 'o' || c == 'l') {
            music_app_on_key(c == 'u' ? BSP_BTN_UP : c == 'd' ? BSP_BTN_DOWN : BSP_BTN_OK,
                             c == 'l' ? BSP_BTN_LONG : BSP_BTN_CLICK, NULL);
        }
    }
}
#endif

void music_app_start(void)
{
    if (s_started) {
        return;
    }

    load_settings();
    (void)mount_music_fs();
    s_local_count = music_library_boot_local(s_local, MUSIC_LOCAL_MAX, s_fs_mounted);
    sync_selection();
    bsp_audio_set_volume(s_volume * 10u);

    // Allocate the Opus decoder before Wi-Fi, TLS and the streaming ring
    // buffer take their blocks; a later allocation needs one large contiguous
    // region and failed on a real device with OPUS_ALLOC_FAIL.
    int decoder_error = OPUS_OK;
    s_decoder = opus_decoder_create(MUSIC_SAMPLE_RATE, MUSIC_CHANNELS, &decoder_error);
    if (!s_decoder) {
        ESP_LOGE(TAG, "Opus 解码器初始化失败: %d，播放不可用", decoder_error);
    } else {
        ESP_LOGI(TAG, "Opus 解码器就绪: %d 字节, 内部最大连续块 %u, DMA 可用 %u",
                 opus_decoder_get_size(MUSIC_CHANNELS),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    }

    s_app_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_event_t));
    s_player_queue = xQueueCreate(PLAYER_QUEUE_DEPTH, sizeof(player_command_t));
    s_settings_queue = xQueueCreate(SETTINGS_QUEUE_DEPTH, sizeof(settings_update_t));
    if (!s_app_queue || !s_player_queue || !s_settings_queue) {
        ESP_LOGE(TAG, "音乐播放器队列创建失败");
        return;
    }

    if (music_library_start(library_callback, s_fs_mounted) != ESP_OK) {
        ESP_LOGE(TAG, "曲库后台任务创建失败");
    }

    if (xTaskCreate(input_worker, "music_input", 6144, NULL, 5, &s_input_task) != pdPASS ||
        xTaskCreate(settings_worker, "music_settings", 3072, NULL, 3,
                    &s_settings_task) != pdPASS) {
        ESP_LOGE(TAG, "音乐播放器任务创建失败");
        return;
    }

    s_player_task = xTaskCreateStatic(player_worker, "music_audio",
                                      ARRAY_COUNT(s_player_stack), NULL, 6,
                                      s_player_stack, &s_player_tcb);
    if (!s_player_task) {
        ESP_LOGE(TAG, "音频任务创建失败");
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = blank_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_blank",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &s_blank_timer) != ESP_OK) {
        ESP_LOGE(TAG, "黑屏计时器创建失败");
    }

    s_menu_selected = 0;
    s_menu_top = 0;
    s_current_song = -1;
    s_playing = false;
    s_paused = false;
    s_screen_awake = true;
    s_shuffle_position = -1;

    if (bsp_lvgl_lock(1000)) {
        build_menu_locked();
        bsp_lvgl_unlock();
    }
    s_started = true;
#if CONFIG_MUSIC_TEST_CONSOLE
    if (xTaskCreate(test_console, "music_test", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "Test console unavailable");
#endif
    ESP_LOGI(TAG, "四项首页设备固件就绪: 在线/本地/缓存/设置");
    ESP_LOGI(TAG, "儿童音乐播放器就绪: %u 首, 音量 %u/10, 模式 %s",
             (unsigned)s_local_count, (unsigned)s_volume,
             s_mode_names[s_mode]);
}
