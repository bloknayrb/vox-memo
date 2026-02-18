/**
 * AXP2101 PMIC — minimal driver for battery monitoring.
 *
 * Reads battery state-of-charge from the built-in fuel gauge
 * and VBUS presence for USB plug detection.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

#define AXP2101_I2C_ADDR  0x34

/**
 * Initialize AXP2101 on the shared I2C bus. Enables the fuel gauge.
 */
esp_err_t axp2101_init(i2c_master_bus_handle_t i2c_bus);

/**
 * Get battery state-of-charge percentage (0–100).
 * Returns -1 if no battery is detected or read fails.
 */
int axp2101_get_battery_percent(void);

/**
 * Check if VBUS (USB power) is present.
 * True when plugged in, even if battery is fully charged.
 */
bool axp2101_is_vbus_present(void);
