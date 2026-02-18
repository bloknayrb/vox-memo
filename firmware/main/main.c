/**
 * Vox Memo — Voice-to-Obsidian capture device
 *
 * ESP32-C6-Touch-AMOLED-1.8 firmware
 * Press-to-talk → record to flash → auto-sync to PC → Obsidian Inbox/
 */

#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "display.h"
#include "imu.h"
#include "network.h"
#include "touch.h"

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

    // Initialize NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Mount flash storage
    init_littlefs();

    // Initialize subsystems
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(imu_init());

    // Log heap after all subsystem init — critical for SRAM budget validation
    ESP_LOGI(TAG, "Free heap after init: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Set timezone to US Eastern (EST5EDT)
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Start Wi-Fi (non-blocking — connects in background)
    ESP_ERROR_CHECK(network_init());

    ESP_LOGI(TAG, "Free heap after Wi-Fi: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Start background sync task
    xTaskCreate(network_sync_task, "sync", 8192, NULL, 3, NULL);

    // Update initial queue badge
    display_update_queue_badge(audio_get_memo_count());

    ESP_LOGI(TAG, "=== Vox Memo ready ===");

    // Main loop: LVGL rendering + input polling + status updates
    int loop_count = 0;
    while (1) {
        // Poll inputs (buttons + touch)
        touch_poll();

        // Periodic status updates (~once per second)
        if (loop_count % 100 == 0) {
            // Update clock
            time_t now;
            time(&now);
            struct tm *t = localtime(&now);
            display_update_time(t->tm_hour, t->tm_min);

            // Update Wi-Fi indicator
            display_update_wifi(network_is_connected());

            // IMU wake/sleep (Phase 6)
            // if (imu_face_down()) { display off } else if (imu_picked_up()) { display on }
        }

        loop_count++;
        vTaskDelay(pdMS_TO_TICKS(10));  // ~100Hz main loop
    }
}
