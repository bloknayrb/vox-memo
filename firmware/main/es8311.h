/**
 * ES8311 Audio Codec — minimal driver for 16kHz/16-bit voice recording + playback.
 *
 * Uses the new i2c_master bus API (ESP-IDF 5.x). The I2C bus is shared with
 * the FT3168 touch controller — created in display.c, handle obtained via
 * display_get_i2c_handle().
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#define ES8311_I2C_ADDR  0x18  /* CS/AD0 pin low (default on Waveshare board) */

/* MIC PGA gain — each step is 3 dB */
#define ES8311_MIC_GAIN_0DB   0x00
#define ES8311_MIC_GAIN_6DB   0x02
#define ES8311_MIC_GAIN_12DB  0x04
#define ES8311_MIC_GAIN_18DB  0x06
#define ES8311_MIC_GAIN_24DB  0x08  /* Good default for voice */
#define ES8311_MIC_GAIN_30DB  0x0A
#define ES8311_MIC_GAIN_33DB  0x0B

/**
 * Initialize ES8311 for 16kHz/16-bit recording.
 * Configures clocks, I2S format, ADC path, and mic preamp.
 *
 * @param i2c_bus   I2C master bus handle (from display_get_i2c_handle())
 * @param mic_gain  MIC PGA gain constant (e.g. ES8311_MIC_GAIN_24DB)
 */
esp_err_t es8311_init(i2c_master_bus_handle_t i2c_bus, uint8_t mic_gain);

/**
 * Enable the DAC output path for playback through external PA.
 */
esp_err_t es8311_enable_dac(void);

/**
 * Mute/unmute the DAC output.
 */
esp_err_t es8311_mute_dac(bool mute);

/**
 * Power down ADC, DAC, and charge pump to save ~3-5 mA.
 * Register state (clocks, bias, LDO) is preserved for fast resume.
 */
esp_err_t es8311_suspend(void);

/**
 * Restore ADC, DAC, and charge pump from suspend.
 * Returns error if any register write fails (avoids silent ADC/DAC bugs).
 */
esp_err_t es8311_resume(void);
