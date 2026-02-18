#include "touch.h"

#include "audio.h"
#include "display.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Button state tracking
static bool btn_record_held = false;
static int64_t btn_record_press_time = 0;

// Debounce
#define DEBOUNCE_MS 50

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

    // TODO: Initialize FT3168 touch controller via I2C
    // The Waveshare BSP should provide touch driver init.
    // Touch coordinates are fed into LVGL's input driver.

    // TODO: Initialize AXP2101 PMIC I2C for PWR button polling
    // Read AXP2101 IRQ status register to detect PKEY press/release.

    ESP_LOGI(TAG, "Touch/button input initialized");
    return ESP_OK;
}

void touch_poll(void) {
    // --- Record button (GPIO9 BOOT, active LOW) ---
    bool pressed = (gpio_get_level(BTN_RECORD_GPIO) == 0);

    if (pressed && !btn_record_held) {
        // Button just pressed — start recording
        btn_record_held = true;
        if (!audio_is_recording()) {
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
            // Update queue badge after a brief delay for file finalization
            vTaskDelay(pdMS_TO_TICKS(200));
            display_update_queue_badge(audio_get_memo_count());
        }
    }

    // Update recording display while held
    if (btn_record_held && audio_is_recording()) {
        display_update_recording(audio_get_recording_elapsed(), AUDIO_MAX_DURATION_SEC);
    }

    // --- PWR button (AXP2101 PKEY) — cycle screens ---
    // TODO: Poll AXP2101 IRQ status register via I2C
    // On PKEY short press: display_next_screen()

    // --- Touch (FT3168) ---
    // TODO: Read touch coordinates from FT3168 via I2C
    // Feed into LVGL input driver for list scrolling, tap-to-play, etc.
}
