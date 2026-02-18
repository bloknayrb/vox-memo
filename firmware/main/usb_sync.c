/**
 * USB Sync — memo transfer via USB Serial/JTAG.
 *
 * Runs a FreeRTOS task that does blocking reads on the USB Serial/JTAG
 * port, parsing simple text commands and responding with file data.
 */

#include "usb_sync.h"
#include "audio.h"
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_sync";

#define USB_RX_BUF_SIZE  1024
#define USB_TX_BUF_SIZE  1024
#define CMD_BUF_SIZE     320   // Enough for "GET /memos/<filename>\n"

static volatile bool sync_active = false;

static int usb_write_str(const char *str)
{
    size_t len = strlen(str);
    return usb_serial_jtag_write_bytes(str, len, pdMS_TO_TICKS(1000));
}

static void handle_list(void)
{
    int count = 0;
    char **memos = audio_list_memos(&count);

    for (int i = 0; i < count; i++) {
        struct stat st;
        long size = 0;
        if (stat(memos[i], &st) == 0) {
            size = (long)st.st_size;
        }
        // Extract filename from path
        const char *name = strrchr(memos[i], '/');
        name = name ? name + 1 : memos[i];

        char line[128];
        snprintf(line, sizeof(line), "FILE %s %ld\n", name, size);
        usb_write_str(line);

        free(memos[i]);
    }
    if (memos) free(memos);

    usb_write_str("END\n");
}

static void handle_get(const char *filename)
{
    char path[280];
    snprintf(path, sizeof(path), MEMO_BASE_PATH "/%s", filename);

    struct stat st;
    if (stat(path, &st) != 0) {
        usb_write_str("ERR File not found\n");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        usb_write_str("ERR Cannot open file\n");
        return;
    }

    long size = (long)st.st_size;
    char header[64];
    snprintf(header, sizeof(header), "DATA %ld\n", size);
    usb_write_str(header);

    // Send file contents in chunks
    uint8_t buf[512];
    uint32_t crc = 0xFFFFFFFF;
    size_t total_sent = 0;

    while (total_sent < (size_t)size) {
        size_t to_read = sizeof(buf);
        if (to_read > (size_t)size - total_sent) {
            to_read = (size_t)size - total_sent;
        }
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) break;

        // Update CRC
        for (size_t i = 0; i < n; i++) {
            crc ^= buf[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }

        usb_serial_jtag_write_bytes((const char *)buf, n, pdMS_TO_TICKS(5000));
        total_sent += n;
    }
    fclose(f);
    crc = ~crc;

    char crc_line[32];
    snprintf(crc_line, sizeof(crc_line), "CRC32 %08lX\n", (unsigned long)crc);
    usb_write_str(crc_line);

    ESP_LOGI(TAG, "Sent %s (%zu bytes, CRC=%08lX)", filename, total_sent, (unsigned long)crc);
}

static void handle_delete(const char *filename)
{
    char path[280];
    snprintf(path, sizeof(path), MEMO_BASE_PATH "/%s", filename);

    if (remove(path) == 0) {
        usb_write_str("OK\n");
        ESP_LOGI(TAG, "Deleted via USB: %s", filename);
    } else {
        usb_write_str("ERR Delete failed\n");
    }
}

static void usb_sync_task(void *arg)
{
    (void)arg;
    char cmd_buf[CMD_BUF_SIZE];
    int cmd_pos = 0;

    ESP_LOGI(TAG, "USB sync task started — listening for commands");

    while (1) {
        // Blocking read with timeout — allows task to yield
        char c;
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (c == '\n' || c == '\r') {
            if (cmd_pos == 0) continue;  // Ignore empty lines
            cmd_buf[cmd_pos] = '\0';
            cmd_pos = 0;

            // Parse command
            sync_active = true;

            if (strcmp(cmd_buf, "LIST") == 0) {
                handle_list();
            } else if (strncmp(cmd_buf, "GET ", 4) == 0) {
                handle_get(cmd_buf + 4);
            } else if (strncmp(cmd_buf, "DELETE ", 7) == 0) {
                handle_delete(cmd_buf + 7);
            } else if (strcmp(cmd_buf, "PING") == 0) {
                usb_write_str("PONG\n");
            } else {
                usb_write_str("ERR Unknown command\n");
            }

            sync_active = false;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

esp_err_t usb_sync_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_RX_BUF_SIZE,
        .tx_buffer_size = USB_TX_BUF_SIZE,
    };
    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB Serial/JTAG driver: %s", esp_err_to_name(ret));
        return ret;
    }

    xTaskCreate(usb_sync_task, "usb_sync", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "USB sync initialized");
    return ESP_OK;
}

bool usb_sync_in_progress(void)
{
    return sync_active;
}
