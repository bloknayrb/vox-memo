#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Button GPIO pins
// BOOT button on ESP32-C6 is GPIO9 (NOT GPIO0 like classic ESP32)
// GPIO0 on this board = PA_CTRL (speaker amp enable) — do not use as button input
#define BTN_RECORD_GPIO  GPIO_NUM_9

/**
 * Initialize button GPIOs, touch controller (FT3168), and input state machine.
 * Registers interrupt-driven ISRs on GPIO9 (ANYEDGE) and GPIO15 (NEGEDGE).
 */
esp_err_t touch_init(void);

/**
 * Get the GPIO event queue populated by ISR handlers.
 * Main loop blocks on this queue; CPU enters light sleep during wait.
 */
QueueHandle_t touch_get_event_queue(void);

/**
 * Process a GPIO event from the queue.
 * Handles:
 *   - GPIO9 (BTN_RECORD_GPIO): press/release state machine → record/sync
 *   - GPIO15 (BSP_TOUCH_INT): un-dim display (does NOT wake from SLPIN)
 */
void touch_process_gpio_event(uint32_t gpio_num);

/**
 * Periodic tick (call once per second from main loop).
 * Updates recording display while button held, checks deferred badge timer.
 */
void touch_tick(void);
