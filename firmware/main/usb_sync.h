/**
 * USB Sync — transfer memos via USB Serial/JTAG when WiFi is unavailable.
 *
 * Uses the ESP32-C6 built-in USB Serial/JTAG peripheral with a simple
 * text+binary protocol. A companion Python script (server/usb_sync.py)
 * reads memos and POSTs them to the existing /memo HTTP endpoint.
 *
 * Protocol:
 *   PC→Device: "LIST\n"          Device→PC: "FILE <name> <size>\n"... "END\n"
 *   PC→Device: "GET <name>\n"    Device→PC: "DATA <size>\n" <raw bytes> "CRC32 <hex>\n"
 *   PC→Device: "DELETE <name>\n" Device→PC: "OK\n" / "ERR <msg>\n"
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the USB sync subsystem. Installs usb_serial_jtag driver
 * and starts the command-processing task.
 */
esp_err_t usb_sync_init(void);

/**
 * Returns true while a USB sync transfer is in progress.
 * Used to block WiFi sync and recording during USB transfers.
 */
bool usb_sync_in_progress(void);
