#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>

// Waveshare ESP32-C6-Touch-AMOLED-1.8 — SH8601 QSPI display
// Resolution: 368 x 448
#define DISP_WIDTH   368
#define DISP_HEIGHT  448

// QSPI pins (from C6-2.06 reference — same SH8601 display IC)
#define BSP_LCD_CS      GPIO_NUM_5
#define BSP_LCD_PCLK    GPIO_NUM_0
#define BSP_LCD_DATA0   GPIO_NUM_1
#define BSP_LCD_DATA1   GPIO_NUM_2
#define BSP_LCD_DATA2   GPIO_NUM_3
#define BSP_LCD_DATA3   GPIO_NUM_4
#define BSP_LCD_RST     GPIO_NUM_11
#define BSP_LCD_SPI_NUM SPI2_HOST

// Touch (FT3168 via FT5x06 driver)
#define BSP_TOUCH_INT   GPIO_NUM_15
#define BSP_TOUCH_RST   GPIO_NUM_10

// I2C bus (shared with audio codec)
#define BSP_I2C_SCL     GPIO_NUM_7
#define BSP_I2C_SDA     GPIO_NUM_8

// Sage green accent color
// In RGB565: R=0x5C>>3=0x0B, G=0x99>>2=0x26, B=0x7C>>3=0x0F
#define COLOR_SAGE_GREEN  0x5B2F  // Approximate RGB565 for #5C997C

// Screen IDs
typedef enum {
    SCREEN_IDLE,
    SCREEN_RECORDING,
    SCREEN_QUEUE,
    SCREEN_SYNC_CONFIRM,
    SCREEN_COUNT,
} screen_id_t;

/**
 * Initialize SH8601 AMOLED display, LVGL, and create all screens.
 */
esp_err_t display_init(void);

/**
 * Switch to a specific screen.
 */
void display_show_screen(screen_id_t screen);

/**
 * Cycle to the next screen (for Button 2).
 */
void display_next_screen(void);

/**
 * Update idle screen elements.
 */
void display_update_time(int hour, int min);
void display_update_wifi(bool connected);
void display_update_queue_badge(int count);
void display_update_battery(int percent);  // Phase 6: called by AXP2101 driver

/**
 * Update recording screen.
 */
void display_update_recording(int elapsed_sec, int max_sec);

/**
 * Show sync confirmation with note title.
 * Auto-returns to idle screen after 3 seconds.
 */
void display_show_sync_result(const char *title, bool success);

/**
 * Refresh the memo list on SCREEN_QUEUE from flash contents.
 * Call after recording, deletion, or sync to keep UI current.
 */
void display_refresh_memo_list(void);

/**
 * Called by audio when playback finishes — resets play button state.
 */
void display_memo_playback_done(void);

/**
 * Get the I2C bus handle (shared between touch and audio codec).
 */
void *display_get_i2c_handle(void);

// Note: LVGL tick and task handling are managed by esp_lvgl_port.
// No manual display_tick() or display_task_handler() needed.
