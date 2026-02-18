#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Server port (home and work IPs defined in network.c)
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
 * Background sync task — call from a FreeRTOS task.
 * Continuously checks for queued memos and uploads when on Wi-Fi.
 */
void network_sync_task(void *arg);
