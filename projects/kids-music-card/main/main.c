// AI Passport offline children's music player entry point.
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "music_app.h"
#include "sdkconfig.h"
#if CONFIG_MUSIC_ONLINE_P0
#include "music_http_source.h"
#include "music_network.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 儿童音乐播放器启动");

    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Do not erase automatically: existing NVS may contain unrelated
        // application data.  The player will use defaults and report a save
        // error until the partition is repaired deliberately.
        ESP_LOGE(TAG, "NVS 分区不可用: %s", esp_err_to_name(nvs_error));
    } else if (nvs_error != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_error));
    }

    (void)bsp_i2c_init();
    (void)bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG,
                 "显示/LVGL 初始化失败，检查 LCD 引脚 MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    const esp_err_t audio_error = bsp_audio_init();
    if (audio_error != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败，歌曲播放不可用: %s",
                 esp_err_to_name(audio_error));
    }
    const esp_err_t battery_error = bsp_battery_init();
    if (battery_error != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用: %s", esp_err_to_name(battery_error));
    }

#if CONFIG_MUSIC_ONLINE_P0
    // Initialize bounded HTTP resources, but never wait for a network at boot.
    const esp_err_t http_error = music_http_source_service_init();
    if (http_error != ESP_OK) {
        ESP_LOGW(TAG, "HTTP source service unavailable: %s", esp_err_to_name(http_error));
    }
#endif

    // Create the application's event queue before registering the button
    // callback.  Button callbacks only enqueue events and return immediately.
    music_app_start();
    const esp_err_t button_error = bsp_button_init(music_app_on_key, NULL);
    if (button_error != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败，无法操作播放器: %s",
                 esp_err_to_name(button_error));
    }
#if CONFIG_MUSIC_ONLINE_P0
    // Menu and buttons are already available; missing credentials do not stop
    // local playback. NVS and driver initialization are not UI operations.
    const esp_err_t network_error = music_network_start_saved();
    if (network_error != ESP_OK) {
        ESP_LOGW(TAG, "Network profile/start unavailable: %s", esp_err_to_name(network_error));
    }
#endif
}
