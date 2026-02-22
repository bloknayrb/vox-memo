/**
 * Vox Memo — Voice-to-Obsidian capture device
 *
 * ESP32-C6-Touch-AMOLED-1.47 firmware
 * Press-to-talk → record to flash → auto-sync to PC → Obsidian Inbox/
 */

#include <stdio.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "axp2101.h"
#include "display.h"
#include "network.h"
#include "settings.h"
#include "touch.h"
#include "usb_sync.h"

static const char *TAG = "vox_memo";

static void init_littlefs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/memos",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS: %zu KB used / %zu KB total", used / 1024, total / 1024);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Vox Memo starting ===");

    // Log initial heap for SRAM budget validation
    ESP_LOGI(TAG, "Free heap at boot: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Initialize NVS (required for Wi-Fi and settings)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load user settings from NVS (before display so brightness applies at init)
    ESP_ERROR_CHECK(settings_init());

    // Enable CPU frequency scaling (DFS): 160MHz max, 80MHz min, no light sleep yet
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,  // Phase 3 enables this
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    // Mount flash storage
    init_littlefs();

    // Initialize subsystems
    ESP_ERROR_CHECK(display_init());
    display_set_brightness(settings_get()->brightness);

    // Initialize AXP2101 PMIC (battery + VBUS monitoring)
    i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)display_get_i2c_handle();
    if (i2c_bus) {
        esp_err_t axp_ret = axp2101_init(i2c_bus);
        if (axp_ret != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 init failed — battery monitoring unavailable");
        }
    }

    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(touch_init());

    // Log heap after all subsystem init — critical for SRAM budget validation
    ESP_LOGI(TAG, "Free heap after init: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Set timezone to US Eastern (EST5EDT)
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Initialize USB sync (memo transfer via USB cable)
    esp_err_t usb_ret = usb_sync_init();
    if (usb_ret != ESP_OK) {
        ESP_LOGW(TAG, "USB sync init failed — USB transfer unavailable");
    }

    // Start Wi-Fi (non-blocking — connects in background)
    ESP_ERROR_CHECK(network_init());

    ESP_LOGI(TAG, "Free heap after Wi-Fi: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Start background sync task
    xTaskCreate(network_sync_task, "sync", 8192, NULL, 3, NULL);

    // Update initial queue badge
    display_update_queue_badge(audio_get_memo_count());

    ESP_LOGI(TAG, "=== Vox Memo ready ===");

    // Main loop: LVGL rendering + input polling + status updates
    uint32_t loop_count = 0;
    bool prev_vbus = false;
    while (1) {
        // Poll inputs (buttons + touch)
        touch_poll();

        // Periodic status updates (~once per second at 20Hz)
        if (loop_count % 20 == 0) {
            // Update clock
            time_t now;
            time(&now);
            struct tm t;
            localtime_r(&now, &t);
            display_update_time(t.tm_hour, t.tm_min);

            // Update Wi-Fi indicator
            if (network_is_connected()) {
                display_update_wifi(WIFI_DISPLAY_CONNECTED, network_get_ssid());
            } else if (network_is_suspended()) {
                display_update_wifi(WIFI_DISPLAY_SUSPENDED, NULL);
            } else if (network_is_connecting()) {
                display_update_wifi(WIFI_DISPLAY_CONNECTING, NULL);
            } else {
                display_update_wifi(WIFI_DISPLAY_DISCONNECTED, NULL);
            }

            // Update battery every ~10 seconds (200 × 50ms = 10s)
            if (loop_count % 200 == 0) {
                int batt = axp2101_get_battery_percent();
                if (batt >= 0) {
                    display_update_battery(batt, axp2101_is_vbus_present());
                }
            }

            // Detect USB plug-in → wake display
            bool cur_vbus = axp2101_is_vbus_present();
            if (cur_vbus && !prev_vbus) {
                display_note_activity();
            }
            prev_vbus = cur_vbus;

            // Advance inactivity timer (skip while recording to keep screen on)
            if (!audio_is_recording()) {
                display_tick_inactivity();
            }
        }

        loop_count++;
        vTaskDelay(pdMS_TO_TICKS(50));  // ~20Hz main loop
    }
}
