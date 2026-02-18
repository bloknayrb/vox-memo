#include "network.h"

#include "audio.h"
#include "display.h"
#include "usb_sync.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_netif_sntp.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "network";

// Wi-Fi credentials and server IPs from secrets.h (gitignored)
#include "secrets.h"

// Multi-SSID entry — each network has its own SSID, password, and server IP
typedef struct {
    const char *ssid;
    const char *password;
    const char *server_ip;
} wifi_entry_t;

static const wifi_entry_t wifi_entries[] = {
    {WIFI_SSID,         WIFI_PASS_HOME,    SERVER_IP_HOME},
#ifdef WIFI_SSID_HOTSPOT
    {WIFI_SSID_HOTSPOT, WIFI_PASS_HOTSPOT, SERVER_IP_HOTSPOT},
#endif
};
#define WIFI_ENTRY_COUNT (sizeof(wifi_entries) / sizeof(wifi_entries[0]))

static int wifi_entry_idx = 0;

// Active server IP (set from current wifi entry or health check)
static char active_server_ip[16] = SERVER_IP_HOME;

// Track whether SNTP has been initialized
static bool sntp_started = false;

// WiFi state machine — prevents reconnect timer from firing during SSID transitions
typedef enum {
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_TRANSITIONING,  // stop/start cycle in progress
    WIFI_STATE_SUSPENDED,      // WiFi off to save power (queue empty)
} wifi_state_t;

static wifi_state_t wifi_state = WIFI_STATE_IDLE;

// Event group for Wi-Fi connection tracking
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT  BIT0

// One-shot timer for reconnect delay (avoids blocking the system event task)
static esp_timer_handle_t reconnect_timer = NULL;

static void try_next_entry(void);

static void set_wifi_config(const wifi_entry_t *entry) {
    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, entry->ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, entry->password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

static void reconnect_timer_cb(void *arg) {
    if (wifi_state == WIFI_STATE_TRANSITIONING) return;  // SSID switch in progress
    wifi_state = WIFI_STATE_CONNECTING;
    esp_wifi_connect();
}

static void schedule_reconnect(void) {
    if (reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .name = "wifi_reconnect",
        };
        esp_timer_create(&timer_args, &reconnect_timer);
    } else {
        esp_timer_stop(reconnect_timer);
    }
    esp_timer_start_once(reconnect_timer, 2000000 /* 2s in µs */);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_state = WIFI_STATE_CONNECTING;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        display_update_wifi(false, NULL);

        if (wifi_state == WIFI_STATE_TRANSITIONING || wifi_state == WIFI_STATE_SUSPENDED) {
            // Expected disconnect during SSID switch or power-save suspend
            return;
        }

        if (disc->reason == WIFI_REASON_AUTH_FAIL ||
            disc->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            disc->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "WiFi failed (reason=%d), trying next network", disc->reason);
            try_next_entry();
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), reconnecting in 2s...", disc->reason);
            schedule_reconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected to '%s', IP: " IPSTR,
                 wifi_entries[wifi_entry_idx].ssid, IP2STR(&event->ip_info.ip));
        wifi_state = WIFI_STATE_IDLE;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        display_update_wifi(true, wifi_entries[wifi_entry_idx].ssid);

        // Set active server IP from the current WiFi entry
        strncpy(active_server_ip, wifi_entries[wifi_entry_idx].server_ip,
                sizeof(active_server_ip) - 1);

        // Start or restart SNTP after network change
        if (!sntp_started) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            sntp_started = true;
            ESP_LOGI(TAG, "SNTP time sync started");
        } else {
            esp_netif_sntp_start();
            ESP_LOGI(TAG, "SNTP restarted after network change");
        }
    }
}

static void try_next_entry(void) {
    wifi_entry_idx = (wifi_entry_idx + 1) % WIFI_ENTRY_COUNT;
    const wifi_entry_t *entry = &wifi_entries[wifi_entry_idx];

    ESP_LOGI(TAG, "Switching to '%s' (entry %d/%d)",
             entry->ssid, wifi_entry_idx + 1, (int)WIFI_ENTRY_COUNT);

    // Full stop/start cycle required when changing SSID
    wifi_state = WIFI_STATE_TRANSITIONING;
    esp_wifi_disconnect();
    esp_wifi_stop();

    set_wifi_config(entry);
    esp_wifi_start();  // triggers WIFI_EVENT_STA_START → esp_wifi_connect()
}

void network_suspend(void) {
    if (wifi_state == WIFI_STATE_SUSPENDED) return;
    wifi_state = WIFI_STATE_SUSPENDED;
    esp_wifi_disconnect();
    esp_wifi_stop();
    display_update_wifi(false, NULL);
    ESP_LOGI(TAG, "WiFi suspended (queue empty)");
}

void network_wake(void) {
    if (wifi_state != WIFI_STATE_SUSPENDED) return;
    wifi_state = WIFI_STATE_CONNECTING;
    esp_wifi_start();  // triggers STA_START handler → esp_wifi_connect()
    ESP_LOGI(TAG, "WiFi waking (memos queued)");
}

esp_err_t network_init(void) {
    wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Start with first WiFi entry
    wifi_entry_idx = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    set_wifi_config(&wifi_entries[wifi_entry_idx]);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialized (%d networks), connecting to '%s'...",
             (int)WIFI_ENTRY_COUNT, wifi_entries[wifi_entry_idx].ssid);
    return ESP_OK;
}

bool network_is_connected(void) {
    return (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

const char *network_get_ssid(void) {
    if (!network_is_connected()) return NULL;
    return wifi_entries[wifi_entry_idx].ssid;
}

// HTTP response buffer for parsing server response
static char http_response_buf[512];
static int http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy_len = evt->data_len;
        if (http_response_len + copy_len < (int)sizeof(http_response_buf) - 1) {
            memcpy(http_response_buf + http_response_len, evt->data, copy_len);
            http_response_len += copy_len;
        }
    }
    return ESP_OK;
}

static esp_err_t probe_ip(const char *ip) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/health", ip, SERVER_PORT);
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 2000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

esp_err_t network_check_server(void) {
    // Probe the server IP associated with the current WiFi network first,
    // then try all other entries as fallback
    if (probe_ip(active_server_ip) == ESP_OK) {
        ESP_LOGI(TAG, "Server found at %s", active_server_ip);
        return ESP_OK;
    }
    for (int i = 0; i < (int)WIFI_ENTRY_COUNT; i++) {
        if (strcmp(wifi_entries[i].server_ip, active_server_ip) == 0) continue;
        if (probe_ip(wifi_entries[i].server_ip) == ESP_OK) {
            strncpy(active_server_ip, wifi_entries[i].server_ip, sizeof(active_server_ip) - 1);
            ESP_LOGI(TAG, "Server found at %s (fallback)", active_server_ip);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "Server not reachable");
    return ESP_FAIL;
}

#define UPLOAD_CHUNK_SIZE 4096

esp_err_t network_upload_memo(const char *filepath, char *title_out, size_t title_len) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", filepath);
        return ESP_FAIL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Extract timestamp from filename (e.g., "20260217_153000" from path)
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;

    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/memo", active_server_ip, SERVER_PORT);

    http_response_len = 0;
    memset(http_response_buf, 0, sizeof(http_response_buf));

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    // Strip .wav extension for timestamp header
    char ts_header[32] = {0};
    const char *dot = strrchr(fname, '.');
    int ts_len = dot ? (dot - fname) : strlen(fname);
    if (ts_len >= (int)sizeof(ts_header)) ts_len = sizeof(ts_header) - 1;
    memcpy(ts_header, fname, ts_len);
    esp_http_client_set_header(client, "X-Memo-Timestamp", ts_header);

    // Stream file in chunks instead of loading entirely into RAM
    esp_err_t err = esp_http_client_open(client, fsize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *chunk = malloc(UPLOAD_CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    long sent = 0;
    while (sent < fsize) {
        int to_read = (fsize - sent > UPLOAD_CHUNK_SIZE) ? UPLOAD_CHUNK_SIZE : (int)(fsize - sent);
        int nread = fread(chunk, 1, to_read, f);
        if (nread <= 0) break;
        int written = esp_http_client_write(client, (const char *)chunk, nread);
        if (written < 0) {
            ESP_LOGE(TAG, "Write failed at byte %ld", sent);
            err = ESP_FAIL;
            break;
        }
        sent += written;
    }
    free(chunk);
    fclose(f);

    if (err != ESP_OK || sent != fsize) {
        ESP_LOGE(TAG, "Upload incomplete: sent %ld / %ld", sent, fsize);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read the response
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > 0 && content_length < (int)sizeof(http_response_buf)) {
        esp_http_client_read(client, http_response_buf, content_length);
        http_response_len = content_length;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Upload failed: err=%d status=%d", err, status);
        return ESP_FAIL;
    }

    http_response_buf[http_response_len] = '\0';
    ESP_LOGI(TAG, "Upload OK: %s", http_response_buf);

    // Parse title from JSON response (simple extraction, no JSON library)
    if (title_out && title_len > 0) {
        const char *t = strstr(http_response_buf, "\"title\":");
        if (t) {
            t = strchr(t, ':') + 1;
            while (*t == ' ' || *t == '"') t++;
            const char *end = strchr(t, '"');
            if (end) {
                int len = end - t;
                if (len >= (int)title_len) len = title_len - 1;
                memcpy(title_out, t, len);
                title_out[len] = '\0';
            }
        }
    }

    return ESP_OK;
}

void network_sync_task(void *arg) {
    ESP_LOGI(TAG, "Sync task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_MS));

        if (!network_is_connected()) continue;
        if (usb_sync_in_progress()) continue;

        if (audio_get_memo_count() == 0) {
            network_suspend();
            continue;
        }

        // Find server before attempting upload
        if (network_check_server() != ESP_OK) continue;

        int count = 0;
        char **memos = audio_list_memos(&count);
        if (!memos || count == 0) continue;

        ESP_LOGI(TAG, "Syncing %d memo(s)...", count);
        int synced = 0;

        for (int i = 0; i < count; i++) {
            char status_buf[48];
            if (count == 1) {
                snprintf(status_buf, sizeof(status_buf), "Syncing...");
            } else {
                snprintf(status_buf, sizeof(status_buf), "Syncing %d/%d...", i + 1, count);
            }
            display_update_sync_status(status_buf);

            char title[64] = {0};
            esp_err_t err = network_upload_memo(memos[i], title, sizeof(title));
            if (err == ESP_OK) {
                audio_delete_memo(memos[i]);
                synced++;
                ESP_LOGI(TAG, "Synced: %s -> \"%s\"", memos[i], title);

                // Show full-screen result after the last memo
                if (i == count - 1) {
                    display_update_sync_status(NULL);
                    display_show_sync_result(title, true);
                }
            } else {
                ESP_LOGW(TAG, "Failed to sync %s, will retry", memos[i]);
                char retry_buf[48];
                snprintf(retry_buf, sizeof(retry_buf), "Retry %d/%d...", i + 1, count);
                display_update_sync_status(retry_buf);
            }
            free(memos[i]);
        }
        free(memos);

        // Clear sync status and update badge
        display_update_sync_status(NULL);
        display_update_queue_badge(audio_get_memo_count());

        // If nothing synced at all, show failure on the full screen
        if (synced == 0) {
            display_show_sync_result("Sync failed", false);
        }

        // Suspend WiFi if queue is now empty after syncing
        if (audio_get_memo_count() == 0) {
            network_suspend();
        }
    }
}
