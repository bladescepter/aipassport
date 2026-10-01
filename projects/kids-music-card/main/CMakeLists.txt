# Source selection may depend on Kconfig; requirements may NOT (IDF expands
# dependencies before loading configuration). Unused network code is not
# linked into the default offline application.
set(music_online_sources "")
if(CONFIG_MUSIC_ONLINE_P0)
    list(APPEND music_online_sources
        "music_http_source.c" "music_network.c" "music_online_profile.c")
endif()

idf_component_register(
    SRCS "main.c"
         "music_app.c"
         "music_source.c"
         "music_file_source.c"
         "music_frame_reader.c"
         "music_stream_buffer.c"
         "music_http_protocol.c"
         ${music_online_sources}
    INCLUDE_DIRS "." "../assets/fonts"
    REQUIRES bsp esp_hw_support esp_timer lvgl nvs_flash opus spiffs
    PRIV_REQUIRES esp_http_client esp_event esp_netif esp_wifi lwip mbedtls heap
)

# Generated LVGL font for the fixed Chinese UI vocabulary and song titles.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/music_font_18.c"
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE=1)

# The application reads the same image that is included in the merged firmware.
# Adding a song only changes assets/music/data and the catalog; menu code is
# independent of the number of catalog entries.
spiffs_create_partition_image(musicfs
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/data"
    FLASH_IN_PROJECT
)
