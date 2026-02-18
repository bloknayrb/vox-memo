#include "touch.h"

#include "audio.h"
#include "display.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Button state tracking
static bool btn_record_held = false;

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

    // AXP2101 PMIC PWR button polling is Phase 6 (pending AXP2101 driver integration).
    // Will read IRQ status register via I2C to detect PKEY press/release.

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
        if (!audio_is_recording() && !display_is_sleeping()) {
            ESP_LOGI(TAG, "Record button pressed — starting recording");
            audio_start_recording();
            display_show_screen(SCREEN_RECORDING);
        }
    } else if (!pressed && btn_record_held) {
        // Button just released — stop recording
        btn_record_held = false;
        if (audio_is_recording()) {
            ESP_LOGI(TAG, "Record button released — stopping recording");
            audio_stop_recording();
            display_show_screen(SCREEN_IDLE);
            // Defer badge update 200ms so file is fully closed before counting
            badge_update_at_us = esp_timer_get_time() + 200000;
        }
    }

    // Update recording display while held
    if (btn_record_held && audio_is_recording()) {
        display_update_recording(audio_get_recording_elapsed(), AUDIO_MAX_DURATION_SEC);
    }

    // --- Deferred queue badge update ---
    if (badge_update_at_us > 0 && esp_timer_get_time() >= badge_update_at_us) {
        badge_update_at_us = 0;
        display_update_queue_badge(audio_get_memo_count());
    }

    // --- PWR button (AXP2101 PKEY) — Phase 6 ---
    // Poll AXP2101 IRQ status register via I2C; on PKEY short press: display_next_screen()

    // --- Touch (FT3168) ---
    // Handled by LVGL input device registered in display_init() — no polling needed here.
    // Read INT pin to detect screen touches for inactivity tracking.
    if (gpio_get_level(BSP_TOUCH_INT) == 0) {
        display_note_activity();
    }
}
