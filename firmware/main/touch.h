#pragma once

#include "esp_err.h"

// Button GPIO pins
// BOOT button on ESP32-C6 is GPIO9 (NOT GPIO0 like classic ESP32)
// GPIO0 on this board = PA_CTRL (speaker amp enable) — do not use as button input
#define BTN_RECORD_GPIO  GPIO_NUM_9

/**
 * Initialize button GPIOs, touch controller (FT3168), and input state machine.
 */
esp_err_t touch_init(void);

/**
 * Poll button and touch state. Call from main loop.
 * Handles:
 *   - Boot button hold/release → start/stop recording
 *   - Touch events → LVGL input driver (gesture swipes, list taps)
 */
void touch_poll(void);
