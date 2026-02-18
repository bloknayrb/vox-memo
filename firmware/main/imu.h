#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize QMI8658 accelerometer via I2C.
 */
esp_err_t imu_init(void);

/**
 * Check if device was just picked up (acceleration spike).
 * Call periodically from main loop.
 */
bool imu_picked_up(void);

/**
 * Check if device is face-down (z-axis inverted).
 */
bool imu_face_down(void);
