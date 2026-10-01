// Child music player application entry points.
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void music_app_start(void);
void music_app_on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user);

#ifdef __cplusplus
}
#endif
