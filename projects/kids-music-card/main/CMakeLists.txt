idf_component_register(
    SRCS "main.c"
         "music_app.c"
    INCLUDE_DIRS "." "../assets/fonts"
    REQUIRES bsp esp_hw_support esp_timer lvgl nvs_flash opus spiffs
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
