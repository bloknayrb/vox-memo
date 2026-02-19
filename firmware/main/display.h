#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

// Waveshare ESP32-C6-Touch-AMOLED-1.47 — SH8601 QSPI display
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
 * Update idle screen elements.
 */
void display_update_time(int hour, int min);
typedef enum {
    WIFI_DISPLAY_DISCONNECTED,
    WIFI_DISPLAY_CONNECTING,
    WIFI_DISPLAY_CONNECTED,
    WIFI_DISPLAY_SUSPENDED,
} wifi_display_state_t;

void display_update_wifi(wifi_display_state_t state, const char *ssid);
void display_update_queue_badge(int count);
void display_update_battery(int percent, bool charging);

/**
 * Update sync progress on idle screen.
 * Pass NULL or "" to hide. Called from network sync task.
 */
void display_update_sync_status(const char *status);

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
 * Briefly show a message on the idle screen prompt label, then restore
 * "Hold to record" after ms milliseconds. No-op if not on idle screen.
 */
void display_show_brief_message(const char *msg, int ms);

/**
 * Get the I2C bus handle (shared between touch and audio codec).
 */
void *display_get_i2c_handle(void);

/**
 * Screen sleep/wake.
 *
 * display_note_activity() — call on any user input to reset the inactivity
 *   timer and wake the display if sleeping.
 * display_tick_inactivity() — call once per second (when not recording) to
 *   advance the inactivity counter and sleep after DISPLAY_SLEEP_TIMEOUT_SEC.
 */
#define DISPLAY_SLEEP_TIMEOUT_SEC  30
#define DISPLAY_DIM_TIMEOUT_SEC   60  // Dim after this many seconds on USB power

void display_sleep(void);
void display_wake(void);
bool display_is_sleeping(void);
void display_note_activity(void);
void display_tick_inactivity(void);

/**
 * Set display brightness (0x00=off, 0xFF=max).
 * Used for dim/wake transitions when on USB power.
 */
void display_set_brightness(uint8_t level);

// Note: LVGL tick and task handling are managed by esp_lvgl_port.
