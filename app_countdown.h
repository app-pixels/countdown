#pragma once
#include "Arduino_GFX_Library.h"

// Called before setup() to inject duration in minutes from setup.txt
void app_countdown_set_config(uint32_t duration_min);

void app_countdown_setup(Arduino_OLED *gfx);
void app_countdown_loop();
