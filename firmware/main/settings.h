#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  volume;           // 0x00–0xFF, default 0xEF
    uint8_t  brightness;       // 0x20–0xFF, default 0xFF
    uint32_t accent_color;     // 0xRRGGBB, default 0x5C997C
    bool     clock_24h;        // default true
    bool     font_large;       // default false
} app_settings_t;

/**
 * Load settings from NVS, applying defaults where no value is stored.
 * Must be called after nvs_flash_init(), before display_init().
 */
esp_err_t settings_init(void);

/**
 * Returns a pointer to the in-memory settings struct.
 * Callers may modify fields directly, then call settings_save().
 */
app_settings_t *settings_get(void);

/**
 * Persist current in-memory settings to NVS.
 */
void settings_save(void);
