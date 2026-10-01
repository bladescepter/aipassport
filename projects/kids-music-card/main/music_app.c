// Offline children's music player for the FoloToy AI Passport.
//
// The input task owns all application state and LVGL objects.  Button callbacks,
// the audio worker and the blank-screen timer only enqueue small events.  The
// audio worker is the sole owner of blocking file/Opus/I2S operations.
#include "music_app.h"

#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "music_catalog.h"
#include "music_font_18.h"
#include "nvs.h"
#include "opus.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "music_app";

#define MUSIC_FS_PARTITION       "musicfs"
#define MUSIC_FS_MOUNT           "/music"
#define MUSIC_SAMPLE_RATE        16000u
#define MUSIC_CHANNELS           1u
#define MUSIC_BITS               16u
#define OPUS_FRAME_SAMPLES       960       // 60 ms at 16 kHz, the decoder maximum
#define OPUS_MAX_PACKET          1500      // larger than a normal 32 kbps music frame
#define AUTO_BLANK_US            (5LL * 1000LL * 1000LL)
#define MENU_VISIBLE_ROWS        8
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
    PAGE_MODE,
    PAGE_PLAYBACK,
} page_t;

typedef enum {
    APP_EVENT_BUTTON = 0,
    APP_EVENT_PLAYER_FINISHED,
    APP_EVENT_PLAYER_ERROR,
    APP_EVENT_BLANK,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    int song;
    int error;
    uint32_t generation;
    uint32_t blank_cookie;
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
static bool s_screen_awake = true;
static int64_t s_ignore_ok_until_us;
static bool s_fs_mounted;
static bool s_started;
static uint8_t s_volume = DEFAULT_VOLUME;
static uint32_t s_play_generation;
static uint32_t s_blank_cookie;

static uint16_t s_shuffle[MUSIC_CATALOG_COUNT];
static int s_shuffle_position = -1;

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
static lv_obj_t *s_mode_rows[MUSIC_MODE_COUNT];
static lv_obj_t *s_mode_labels[MUSIC_MODE_COUNT];
static lv_obj_t *s_play_title;
static lv_obj_t *s_play_status;
static lv_obj_t *s_play_volume;
static lv_obj_t *s_play_mode;

static const char *const s_mode_names[MUSIC_MODE_COUNT] = {
    "顺序播放",
    "随机播放",
    "单曲循环",
};

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
        .max_files = 2,
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
    FILE *file;
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
    if (state->decoder) {
        opus_decoder_destroy(state->decoder);
        state->decoder = NULL;
    }
    if (state->file) {
        fclose(state->file);
        state->file = NULL;
    }
    state->playing = false;
    state->paused = false;
    state->song = -1;
}

static bool player_open(player_state_t *state, const player_command_t *command)
{
    if (!state || !command || command->song < 0 ||
        command->song >= (int)MUSIC_CATALOG_COUNT || !s_fs_mounted) {
        return false;
    }

    char path[sizeof(MUSIC_FS_MOUNT) + 1 + 32];
    const music_track_t *track = &MUSIC_CATALOG[command->song];
    const int path_length = snprintf(path, sizeof(path), "%s/%s",
                                     MUSIC_FS_MOUNT, track->path);
    if (path_length < 0 || path_length >= (int)sizeof(path)) {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "找不到歌曲资源: %s (%s)", track->title, path);
        return false;
    }

    if (bsp_audio_set_format(MUSIC_SAMPLE_RATE, MUSIC_BITS, MUSIC_CHANNELS) != ESP_OK) {
        ESP_LOGE(TAG, "音频格式初始化失败: %s", track->title);
        fclose(file);
        return false;
    }

    int decoder_error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(MUSIC_SAMPLE_RATE,
                                                 MUSIC_CHANNELS,
                                                 &decoder_error);
    if (!decoder) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d", decoder_error);
        fclose(file);
        return false;
    }

    bsp_audio_set_volume(command->volume * 10u);
    state->file = file;
    state->decoder = decoder;
    state->song = command->song;
    state->generation = command->generation;
    state->playing = true;
    state->paused = false;
    ESP_LOGI(TAG, "开始播放: %s", track->title);
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
        break;
    case PLAYER_CMD_PAUSE:
        if (state->playing) {
            state->paused = true;
        }
        break;
    case PLAYER_CMD_RESUME:
        if (state->playing) {
            state->paused = false;
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

    uint8_t packet[OPUS_MAX_PACKET];
    int16_t pcm[OPUS_FRAME_SAMPLES];
    uint8_t packet_length_bytes[2];

    for (;;) {
        if (!state.playing || state.paused) {
            player_command_t command;
            if (xQueueReceive(s_player_queue, &command, portMAX_DELAY) == pdTRUE) {
                player_handle_command(&state, &command);
            }
            continue;
        }

        // A frame is at most 60 ms.  Polling here keeps stop/pause/next-song
        // latency bounded while all blocking I/O remains in this worker.
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

        if (fread(packet_length_bytes, 1, sizeof(packet_length_bytes), state.file) !=
            sizeof(packet_length_bytes)) {
            const int song = state.song;
            const uint32_t generation = state.generation;
            player_close(&state);
            post_player_event(APP_EVENT_PLAYER_FINISHED, song, generation, ESP_OK);
            continue;
        }

        const uint16_t packet_length = (uint16_t)packet_length_bytes[0] |
                                       ((uint16_t)packet_length_bytes[1] << 8);
        if (packet_length == 0 || packet_length > sizeof(packet)) {
            const int song = state.song;
            const uint32_t generation = state.generation;
            ESP_LOGE(TAG, "歌曲 %d 的 Opus 包长度非法: %u", song, packet_length);
            player_close(&state);
            post_player_event(APP_EVENT_PLAYER_ERROR, song, generation, ESP_ERR_INVALID_SIZE);
            continue;
        }
        if (fread(packet, 1, packet_length, state.file) != packet_length) {
            const int song = state.song;
            const uint32_t generation = state.generation;
            player_close(&state);
            post_player_event(APP_EVENT_PLAYER_ERROR, song, generation, ESP_ERR_INVALID_SIZE);
            continue;
        }

        const int samples = opus_decode(state.decoder, packet, packet_length,
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
    lv_obj_t *label = lv_label_create(parent);
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
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
}

static lv_obj_t *make_row(lv_obj_t *screen, int y, const char *text, bool selected,
                          lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_size(row, 232, 29);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row,
                              selected ? lv_color_hex(COLOR_SELECTED)
                                       : lv_color_hex(COLOR_PANEL), 0);

    lv_obj_t *label = make_label(row, text, lv_color_hex(COLOR_TEXT));
    lv_obj_set_pos(label, 10, 0);
    lv_obj_set_size(label, 214, 29);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, selected ? LV_LABEL_LONG_SCROLL_CIRCULAR
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

static int menu_item_count(void)
{
    return (int)MUSIC_CATALOG_COUNT + 1; // last item is the mode selector
}

static void menu_refresh_locked(void)
{
    const int count = menu_item_count();
    if (s_menu_selected < s_menu_top) {
        s_menu_top = s_menu_selected;
    }
    if (s_menu_selected >= s_menu_top + MENU_VISIBLE_ROWS) {
        s_menu_top = s_menu_selected - MENU_VISIBLE_ROWS + 1;
    }
    if (s_menu_top < 0) {
        s_menu_top = 0;
    }

    for (int row = 0; row < MENU_VISIBLE_ROWS; row++) {
        const int item = s_menu_top + row;
        if (item >= count) {
            lv_obj_add_flag(s_menu_rows[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_menu_rows[row], LV_OBJ_FLAG_HIDDEN);
        const bool selected = item == s_menu_selected;
        const char *text = item < (int)MUSIC_CATALOG_COUNT
                         ? MUSIC_CATALOG[item].title : "";
        char mode_text[48];
        if (item == (int)MUSIC_CATALOG_COUNT) {
            snprintf(mode_text, sizeof(mode_text), "播放模式：%s", s_mode_names[s_mode]);
            text = mode_text;
        }
        lv_label_set_text(s_menu_labels[row], text);
        lv_label_set_long_mode(s_menu_labels[row],
                               selected ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                        : LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(s_menu_rows[row],
                                  selected ? lv_color_hex(COLOR_SELECTED)
                                           : lv_color_hex(COLOR_PANEL), 0);
    }
}

static void build_menu_locked(void)
{
    destroy_screen_locked();
    s_page = PAGE_MENU;
    s_screen = make_screen("儿童音乐播放器");
    for (int row = 0; row < MENU_VISIBLE_ROWS; row++) {
        s_menu_rows[row] = make_row(s_screen, 45 + row * 30, "", false,
                                    &s_menu_labels[row]);
    }
    make_footer(s_screen, "上下选择    确定进入");
    menu_refresh_locked();
    lv_screen_load(s_screen);
}

static void mode_refresh_locked(void)
{
    for (int i = 0; i < MUSIC_MODE_COUNT; i++) {
        const bool selected = i == (int)s_mode_selection;
        lv_label_set_text(s_mode_labels[i], s_mode_names[i]);
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
    for (int i = 0; i < MUSIC_MODE_COUNT; i++) {
        s_mode_rows[i] = make_row(s_screen, 65 + i * 42, s_mode_names[i],
                                  i == (int)s_mode_selection, &s_mode_labels[i]);
    }
    make_footer(s_screen, "上下选择    确定保存");
    mode_refresh_locked();
    lv_screen_load(s_screen);
}

static void playback_refresh_locked(void)
{
    if (!s_play_title || !s_play_status || !s_play_volume || !s_play_mode ||
        s_current_song < 0 || s_current_song >= (int)MUSIC_CATALOG_COUNT) {
        return;
    }
    lv_label_set_text(s_play_title, MUSIC_CATALOG[s_current_song].title);
    lv_label_set_text(s_play_status,
                      !s_playing ? "音频未安装" : (s_paused ? "已暂停" : "播放中"));

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
    lv_obj_set_size(s_play_title, 220, 54);
    lv_obj_set_style_text_align(s_play_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_play_title, LV_LABEL_LONG_WRAP);

    s_play_status = make_label(s_screen, "", lv_color_hex(COLOR_ACCENT));
    lv_obj_set_pos(s_play_status, 10, 164);
    lv_obj_set_size(s_play_status, 220, 28);
    lv_obj_set_style_text_align(s_play_status, LV_TEXT_ALIGN_CENTER, 0);

    s_play_volume = make_label(s_screen, "", lv_color_hex(COLOR_MUTED));
    lv_obj_set_pos(s_play_volume, 10, 204);
    lv_obj_set_size(s_play_volume, 220, 28);
    lv_obj_set_style_text_align(s_play_volume, LV_TEXT_ALIGN_CENTER, 0);

    s_play_mode = make_label(s_screen, "", lv_color_hex(COLOR_MUTED));
    lv_obj_set_pos(s_play_mode, 10, 236);
    lv_obj_set_size(s_play_mode, 220, 28);
    lv_obj_set_style_text_align(s_play_mode, LV_TEXT_ALIGN_CENTER, 0);

    make_footer(s_screen, "上下音量    确定暂停/播放    长按返回/亮屏");
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
    if (song < 0 || song >= (int)MUSIC_CATALOG_COUNT) {
        return;
    }

    s_play_generation++;
    s_current_song = song;
    s_playing = true;
    s_paused = false;
    if (wake_screen) {
        s_screen_awake = true;
        bsp_display_backlight(100);
    }
    (void)send_player_command(PLAYER_CMD_START, song, s_volume, s_play_generation);
    if (s_page != PAGE_PLAYBACK) {
        build_playback_page_locked();
    } else {
        playback_refresh_locked();
    }
    blank_timer_start();
}

static void shuffle_for_song(int first_song)
{
    for (int i = 0; i < (int)MUSIC_CATALOG_COUNT; i++) {
        s_shuffle[i] = (uint16_t)i;
    }
    for (int i = (int)MUSIC_CATALOG_COUNT - 1; i > 0; i--) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const uint16_t temp = s_shuffle[i];
        s_shuffle[i] = s_shuffle[j];
        s_shuffle[j] = temp;
    }
    if (first_song >= 0 && first_song < (int)MUSIC_CATALOG_COUNT) {
        int position = 0;
        while (position < (int)MUSIC_CATALOG_COUNT &&
               s_shuffle[position] != (uint16_t)first_song) {
            position++;
        }
        if (position < (int)MUSIC_CATALOG_COUNT) {
            const uint16_t temp = s_shuffle[0];
            s_shuffle[0] = s_shuffle[position];
            s_shuffle[position] = temp;
        }
    }
    s_shuffle_position = 0;
}

static int next_song_locked(void)
{
    if (s_mode == MUSIC_MODE_SINGLE) {
        return s_current_song;
    }
    if (s_mode == MUSIC_MODE_SEQUENTIAL) {
        return (s_current_song + 1) % (int)MUSIC_CATALOG_COUNT;
    }

    if (s_shuffle_position < 0) {
        shuffle_for_song(s_current_song);
    }
    s_shuffle_position++;
    if (s_shuffle_position >= (int)MUSIC_CATALOG_COUNT) {
        shuffle_for_song(-1);
    }
    return (int)s_shuffle[s_shuffle_position];
}

static void stop_to_menu_locked(void)
{
    s_play_generation++;
    s_playing = false;
    s_paused = false;
    s_current_song = -1;
    s_screen_awake = true;
    s_ignore_ok_until_us = esp_timer_get_time() + 500000;
    blank_timer_cancel();
    bsp_display_backlight(100);
    (void)send_player_command(PLAYER_CMD_STOP, -1, s_volume, s_play_generation);
    build_menu_locked();
}

static void handle_volume_locked(int delta)
{
    int next = (int)s_volume + delta;
    if (next < VOLUME_MIN) {
        next = VOLUME_MIN;
    }
    if (next > VOLUME_MAX) {
        next = VOLUME_MAX;
    }
    if (next == (int)s_volume) {
        return;
    }
    s_volume = (uint8_t)next;
    (void)send_player_command(PLAYER_CMD_SET_VOLUME, -1, s_volume,
                              s_play_generation);
    post_settings(false);
    playback_refresh_locked();
    // Deliberately do not turn the backlight on here.  Volume changes while
    // blank must remain silent from the display's point of view.
}

/* -------------------------------------------------------------------------- */
/* Input event handling                                                       */
/* -------------------------------------------------------------------------- */

static void handle_button_locked(bsp_btn_t button, bsp_btn_ev_t event)
{
    // Some button-component versions may report a trailing click after LONG.
    // Ignore only that short release window; do not make the next real menu
    // selection disappear when a long press did not generate a click.
    if (button == BSP_BTN_OK && event == BSP_BTN_CLICK &&
        esp_timer_get_time() < s_ignore_ok_until_us) {
        return;
    }

    if (s_page == PAGE_MENU) {
        if (event != BSP_BTN_CLICK) {
            return;
        }
        const int count = menu_item_count();
        if (button == BSP_BTN_UP) {
            s_menu_selected = (s_menu_selected + count - 1) % count;
            menu_refresh_locked();
        } else if (button == BSP_BTN_DOWN) {
            s_menu_selected = (s_menu_selected + 1) % count;
            menu_refresh_locked();
        } else if (button == BSP_BTN_OK) {
            if (s_menu_selected == (int)MUSIC_CATALOG_COUNT) {
                build_mode_page_locked();
            } else {
                if (s_mode == MUSIC_MODE_RANDOM) {
                    shuffle_for_song(s_menu_selected);
                }
                start_song_locked(s_menu_selected, true);
            }
        }
        return;
    }

    if (s_page == PAGE_MODE) {
        if (event != BSP_BTN_CLICK) {
            return;
        }
        if (button == BSP_BTN_UP) {
            s_mode_selection = (music_mode_t)((s_mode_selection + MUSIC_MODE_COUNT - 1) %
                                               MUSIC_MODE_COUNT);
            mode_refresh_locked();
        } else if (button == BSP_BTN_DOWN) {
            s_mode_selection = (music_mode_t)((s_mode_selection + 1) % MUSIC_MODE_COUNT);
            mode_refresh_locked();
        } else if (button == BSP_BTN_OK) {
            s_mode = s_mode_selection;
            if (s_mode == MUSIC_MODE_RANDOM) {
                shuffle_for_song(-1);
            }
            post_settings(true);
            build_menu_locked();
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
    if (button == BSP_BTN_UP && event == BSP_BTN_CLICK) {
        handle_volume_locked(+1);
    } else if (button == BSP_BTN_DOWN && event == BSP_BTN_CLICK) {
        handle_volume_locked(-1);
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

static void handle_app_event_locked(const app_event_t *event)
{
    if (!event) {
        return;
    }
    switch (event->type) {
    case APP_EVENT_BUTTON:
        handle_button_locked(event->button, event->button_event);
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
            s_playing = false;
            s_paused = false;
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
        if (xQueueReceive(s_app_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!bsp_lvgl_lock(1000)) {
            ESP_LOGW(TAG, "获取 LVGL 锁超时，丢弃应用事件");
            continue;
        }
        handle_app_event_locked(&event);
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

void music_app_start(void)
{
    if (s_started) {
        return;
    }

    load_settings();
    (void)mount_music_fs();
    bsp_audio_set_volume(s_volume * 10u);

    s_app_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_event_t));
    s_player_queue = xQueueCreate(PLAYER_QUEUE_DEPTH, sizeof(player_command_t));
    s_settings_queue = xQueueCreate(SETTINGS_QUEUE_DEPTH, sizeof(settings_update_t));
    if (!s_app_queue || !s_player_queue || !s_settings_queue) {
        ESP_LOGE(TAG, "音乐播放器队列创建失败");
        return;
    }

    if (xTaskCreate(input_worker, "music_input", 4096, NULL, 5, &s_input_task) != pdPASS ||
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
    ESP_LOGI(TAG, "儿童音乐播放器就绪: %u 首, 音量 %u/10, 模式 %s",
             (unsigned)MUSIC_CATALOG_COUNT, (unsigned)s_volume,
             s_mode_names[s_mode]);
}
