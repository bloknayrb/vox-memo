#pragma once

#include "esp_err.h"

// Button GPIO pins
// BOOT button on ESP32-C6 is GPIO9 (NOT GPIO0 like classic ESP32)
// GPIO0 on this board = PA_CTRL (speaker amp enable) — do not use as button input
#define BTN_RECORD_GPIO  GPIO_NUM_9

// PWR button may be AXP2101 PKEY — handled via I2C polling, not GPIO
// This define is for fallback if a second GPIO button is available
#define BTN_CYCLE_GPIO   GPIO_NUM_NC  // Not connected — use AXP2101 PKEY via I2C

/**
 * Initialize button GPIOs, touch controller (FT3168), and input state machine.
 */
esp_err_t touch_init(void);

/**
 * Poll button and touch state. Call from main loop.
 * Handles:
 *   - Button 1 hold/release → start/stop recording
 *   - Button 2 press → cycle screens
 *   - Touch events → LVGL input driver
 */
void touch_poll(void);
