#include "touch.h"

#include "audio.h"
#include "display.h"
#include "network.h"
#include "usb_sync.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Button state tracking
static bool btn_record_held = false;
static bool press_started_recording = false;  // true only if recording actually began
static int64_t press_start_us = 0;            // timestamp of button press

// Deferred badge update: set to non-zero after recording stops, checked each loop tick
static int64_t badge_update_at_us = 0;

esp_err_t touch_init(void) {
    // Configure BOOT button (GPIO9 on ESP32-C6) as input with pull-up
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_RECORD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  // Polling, not interrupt
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // FT3168 touch is initialized in display_init() via esp_lcd_touch (FT5x06 driver).
    // Touch events are handled by the LVGL input device registered in display.c.

    ESP_LOGI(TAG, "Touch/button input initialized");
    return ESP_OK;
}

void touch_poll(void) {
    // --- Record button (GPIO9 BOOT, active LOW) ---
    bool pressed = (gpio_get_level(BTN_RECORD_GPIO) == 0);

    if (pressed && !btn_record_held) {
        // Button just pressed — wake display or start recording
        display_note_activity();
        btn_record_held = true;
        press_start_us = esp_timer_get_time();
        if (display_is_sleeping()) {
            // Just wake the display — no recording, no sync
            press_started_recording = false;
        } else if (usb_sync_in_progress()) {
            display_show_brief_message("USB sync active", 1500);
            press_started_recording = false;
        } else if (!audio_is_recording()) {
            ESP_LOGI(TAG, "Record button pressed — starting recording");
            audio_start_recording();
            display_show_screen(SCREEN_RECORDING);
            press_started_recording = true;
        } else {
            press_started_recording = false;
        }
    } else if (!pressed && btn_record_held) {
        // Button just released
        bool pressed_long = (esp_timer_get_time() - press_start_us) >= 500000;  // 500ms
        btn_record_held = false;
        bool discarded = false;
        audio_stop_recording(&discarded);
        display_show_screen(SCREEN_IDLE);

        if (!press_started_recording) {
            // Pressed while sleeping/blocked — nothing to do
        } else if (!pressed_long) {
            // Short tap — trigger an immediate sync; audio was auto-discarded (<1s)
            if (network_is_connected()) {
                network_trigger_sync();
                display_show_brief_message("Syncing...", 1000);
            } else {
                display_show_brief_message("No WiFi", 1500);
            }
        } else if (audio_was_storage_full()) {
            display_show_brief_message("Storage full", 2000);
        } else if (!discarded) {
            badge_update_at_us = esp_timer_get_time() + 200000;
        } else {
            display_show_brief_message("Too short", 1500);
        }
    }

    // Update recording display while held
    if (btn_record_held && audio_is_recording()) {
        display_update_recording(audio_get_recording_elapsed(), audio_get_recording_max_sec());
    }

    // --- Deferred queue badge update ---
    if (badge_update_at_us > 0 && esp_timer_get_time() >= badge_update_at_us) {
        badge_update_at_us = 0;
        display_update_queue_badge(audio_get_memo_count());
    }

    // --- Touch (FT3168) ---
    // Handled by LVGL input device registered in display_init() — no polling needed here.
    // Read INT pin to detect screen touches for inactivity tracking.
    if (!display_is_sleeping() && gpio_get_level(BSP_TOUCH_INT) == 0) {
        display_note_activity();
    }
}
