#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Server port (IPs defined per-network in network.c)
#define SERVER_PORT       8000
#define SYNC_INTERVAL_MS  30000  // Check for queued memos every 30s

/**
 * Initialize Wi-Fi in station mode and start auto-reconnect.
 * Credentials are stored in NVS (set via provisioning or hardcode for dev).
 */
esp_err_t network_init(void);

/**
 * Returns true if connected to Wi-Fi.
 */
bool network_is_connected(void);

/**
 * Upload a single WAV memo to the PC server.
 * @param filepath  Full path on LittleFS, e.g., "/memos/20260217_153000.wav"
 * @param title_out Buffer to receive the note title from server response (can be NULL)
 * @param title_len Size of title_out buffer
 * @return ESP_OK on successful upload and server acceptance
 */
esp_err_t network_upload_memo(const char *filepath, char *title_out, size_t title_len);

/**
 * Check server health/reachability.
 * @return ESP_OK if server responds to GET /health
 */
esp_err_t network_check_server(void);

/**
 * Returns the SSID of the currently connected network, or NULL if not connected.
 */
const char *network_get_ssid(void);

/**
 * Returns true if WiFi is intentionally suspended (power save).
 */
bool network_is_suspended(void);

/**
 * Returns true if WiFi is currently connecting/transitioning.
 */
bool network_is_connecting(void);

/**
 * Suspend WiFi to save power (queue empty, nothing to sync).
 * Calls esp_wifi_stop() — reconnects automatically via network_wake().
 */
void network_suspend(void);

/**
 * Wake WiFi after suspend (new memo recorded, needs sync).
 * No-op if WiFi is already active.
 */
void network_wake(void);

/**
 * Background sync task — call from a FreeRTOS task.
 * Continuously checks for queued memos and uploads when on Wi-Fi.
 * Interval is read from settings_get()->sync_interval_s each cycle.
 */
void network_sync_task(void *arg);

/**
 * Signal the sync task to run immediately, skipping its current sleep interval.
 * Safe to call from any task or ISR context.
 */
void network_trigger_sync(void);
