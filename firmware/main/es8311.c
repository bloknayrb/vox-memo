/**
 * ES8311 Audio Codec — register init for 16kHz/16-bit/mono voice recording.
 *
 * Clock setup: ESP32 I2S master provides MCLK = 256 * 16kHz = 4.096 MHz.
 * ES8311 is I2S slave. BCLK divider = 4, LRCK divider = 256 → 16kHz.
 *
 * Register values cross-referenced against:
 *   - ES8311 datasheet (Everset)
 *   - espressif/esp-bsp es8311 driver (coeff_div table, es8311_microphone_config)
 */

#include "es8311.h"
#include "settings.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";

/* ES8311 register addresses */
#define REG_RESET    0x00
#define REG_CLK1     0x01
#define REG_CLK2     0x02
#define REG_CLK3     0x03
#define REG_CLK4     0x04  /* DAC oversample rate */
#define REG_CLK5     0x05  /* ADC/DAC clock divider */
#define REG_CLK6     0x06  /* BCLK divider */
#define REG_CLK7     0x07  /* LRCK divider high byte */
#define REG_CLK8     0x08  /* LRCK divider low byte */
#define REG_SDPIN    0x09  /* I2S DAC input format */
#define REG_SDPOUT   0x0A  /* I2S ADC output format */
#define REG_SYS_D    0x0D  /* Power management */
#define REG_SYS_E    0x0E  /* Analog references */
#define REG_SYS_F    0x0F  /* REF/VSEL */
#define REG_SYS_10   0x10  /* Clock enables */
#define REG_SYS_11   0x11  /* HP reference */
#define REG_SYS_12   0x12  /* LDO */
#define REG_SYS_13   0x13  /* HP output power */
#define REG_SYS_14   0x14  /* Analog MIC enable, PGA config */
#define REG_ADC_15   0x15  /* ADC power, PGA, MICBIAS */
#define REG_ADC_16   0x16  /* MIC gain (esp-bsp: 0-7, 6dB steps) */
#define REG_ADC_17   0x17  /* ADC gain / ramp config */
#define REG_ADC_18   0x18  /* ADC EQ */
#define REG_ADC_PGA  0x1B  /* MIC PGA gain */
#define REG_ADC_VOL  0x1C  /* ADC digital volume */
#define REG_DAC_31   0x31  /* DAC power */
#define REG_DAC_VOL  0x32  /* DAC digital volume */
#define REG_DAC_35   0x35  /* DAC ramp rate */
#define REG_DAC_37   0x37  /* DAC mono mix */
#define REG_CHIPID1  0xFD  /* Should read 0x83 */

static i2c_master_dev_handle_t dev_handle = NULL;
static uint8_t s_mic_gain;

static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

static esp_err_t es_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, val, 1, 100);
}

/**
 * Write all configuration registers: clocks, I2S format, system power, ADC path.
 * Called from both init (after reset) and resume (after potential full register loss).
 * Uses module-level s_mic_gain set before first call.
 */
static esp_err_t es8311_configure(void)
{
    /* Clock registers — match esp-bsp coeff_div for {4096000, 16000} */
    esp_err_t clk_ret = ESP_OK;
    clk_ret |= es_write(REG_CLK1, 0x3F);  /* Clock tree enable — all clocks on, ES8311 I2S slave */
    clk_ret |= es_write(REG_CLK2, 0x00);  /* MCLK integer div=1 (passthrough) */
    clk_ret |= es_write(REG_CLK3, 0x10);  /* ADC single-speed, OSR select */
    clk_ret |= es_write(REG_CLK4, 0x10);  /* DAC oversample (was 0x08) */
    clk_ret |= es_write(REG_CLK5, 0x00);  /* DAC single-speed */
    clk_ret |= es_write(REG_CLK6, 0x03);  /* BCLK divider = 4 (was 0x08) */
    clk_ret |= es_write(REG_CLK7, 0x00);  /* LRCK divider high byte (was 0x7F) */
    clk_ret |= es_write(REG_CLK8, 0xFF);  /* LRCK divider low byte → 256 frames → 16kHz (was 0x00) */
    if (clk_ret != ESP_OK) {
        ESP_LOGE(TAG, "Clock config failed");
        return clk_ret;
    }

    /* I2S format: Philips standard, 16-bit — critical for correct audio capture */
    ESP_RETURN_ON_ERROR(es_write(REG_SDPIN,  0x0C), TAG, "I2S DAC format failed");
    ESP_RETURN_ON_ERROR(es_write(REG_SDPOUT, 0x0C), TAG, "I2S ADC format failed");

    /* System power-up */
    ESP_RETURN_ON_ERROR(es_write(REG_SYS_D,  0x01), TAG, "Power-up failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t ret = ESP_OK;
    ret |= es_write(REG_SYS_E,  0x02);  /* Enable analog PGA + ADC modulator */
    ret |= es_write(REG_SYS_F,  0x00);  /* 3.3V reference */
    ret |= es_write(REG_SYS_10, 0x1F);  /* All clock enables: ADC+DAC+reference clocks */
    ret |= es_write(REG_SYS_11, 0x7F);  /* HP voltage reference — needed for analog bias */
    ret |= es_write(REG_SYS_12, 0x00);  /* LDO 1.35V */
    ret |= es_write(REG_SYS_13, 0x10);  /* HP off */
    ret |= es_write(REG_SYS_14, 0x1A);  /* Enable analog MIC input + PGA (was 0xBF — mic path was never connected!) */

    /* ADC path: analog mic (AMIC) mode, ADC on.
     * Matches esp-bsp es8311_microphone_config(handle, false). */
    ret |= es_write(REG_ADC_15, 0x00);  /* ADC on, AMIC mode */
    ret |= es_write(REG_ADC_16, s_mic_gain);  /* MIC gain via esp-bsp register (0-7, 6dB steps) */
    ret |= es_write(REG_ADC_17, 0xC8);  /* ADC gain / ramp config (was 0x00) */
    ret |= es_write(REG_ADC_18, 0x02);  /* EQ bypass (flat response) */
    ret |= es_write(REG_ADC_PGA, s_mic_gain);
    ret |= es_write(REG_ADC_VOL, 0x6A); /* ADC EQ bypass + DC offset cancel (from esp-bsp reference) */
    ret |= es_write(REG_DAC_31,  0x00); /* DAC digital power on (0x60 mutes DSM+DEM) */
    ret |= es_write(REG_DAC_VOL, 0xEF); /* ~-6dB (0xFF=0dB, 0x00=-96dB, step=0.376dB/step) */
    ret |= es_write(REG_DAC_35,  0x20); /* Moderate ramp for pop suppression */
    ret |= es_write(REG_DAC_37,  0x08); /* Normal DAC path */

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "One or more register writes failed during configure");
    }
    vTaskDelay(pdMS_TO_TICKS(50));  /* Analog settling — allow for full register-loss recovery */
    return ret;
}

esp_err_t es8311_init(i2c_master_bus_handle_t i2c_bus, uint8_t mic_gain)
{
    /* Add ES8311 as a device on the shared I2C bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_handle),
                        TAG, "Failed to add ES8311 I2C device");

    /* Verify chip is present */
    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(es_read(REG_CHIPID1, &chip_id), TAG, "I2C read failed");
    if (chip_id != 0x83) {
        ESP_LOGE(TAG, "ES8311 not found (id=0x%02X, expected 0x83)", chip_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ES8311 found (id=0x%02X)", chip_id);

    /* Soft reset — critical: fail fast if I2C is broken */
    ESP_RETURN_ON_ERROR(es_write(REG_RESET, 0x1F), TAG, "Soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es_write(REG_RESET, 0x00), TAG, "Soft reset release failed");
    ESP_RETURN_ON_ERROR(es_write(REG_RESET, 0x80), TAG, "Power-on failed");  /* Set master power-on bit */

    s_mic_gain = mic_gain;
    ESP_RETURN_ON_ERROR(es8311_configure(), TAG, "Configure failed");

    ESP_LOGI(TAG, "ES8311 initialized: 16kHz 16-bit, MIC gain=%ddB", mic_gain * 6);
    return ESP_OK;
}

esp_err_t es8311_set_dac_volume(uint8_t vol)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    return es_write(REG_DAC_VOL, vol);
}

esp_err_t es8311_enable_dac(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t vol = settings_get()->volume;
    es_write(REG_DAC_31, 0x00);   /* DAC power on (0x60 mutes DSM+DEM!) */
    es_write(REG_DAC_VOL, vol);   /* volume from settings (default 0xEF ~-6dB) */
    es_write(REG_DAC_35, 0x20);   /* Moderate ramp (pop suppression) */
    es_write(REG_SYS_14, 0x1A);   /* Enable analog MIC + PGA */
    ESP_LOGI(TAG, "DAC enabled, vol=0x%02X", vol);
    return ESP_OK;
}

esp_err_t es8311_mute_dac(bool mute)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t vol = settings_get()->volume;
    return es_write(REG_DAC_VOL, mute ? 0x00 : vol);
}

esp_err_t es8311_suspend(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(es_write(REG_SYS_14, 0x00), TAG, "analog MIC suspend failed");
    ESP_RETURN_ON_ERROR(es_write(REG_ADC_15, 0x00), TAG, "ADC suspend failed");
    ESP_RETURN_ON_ERROR(es_write(REG_DAC_31, 0x60), TAG, "DAC suspend failed");
    ESP_LOGI(TAG, "ES8311 suspended");
    return ESP_OK;
}

esp_err_t es8311_resume(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(es8311_configure(), TAG, "Configure failed");

    /* Read back key registers to verify I2C writes took effect */
    uint8_t chip_id = 0, clk6 = 0, clk8 = 0, sys14 = 0, adc15 = 0, adc17 = 0;
    uint8_t dac31 = 0, dac_vol = 0;
    es_read(REG_CHIPID1, &chip_id);
    es_read(REG_CLK6,    &clk6);
    es_read(REG_CLK8,    &clk8);
    es_read(REG_SYS_14,  &sys14);
    es_read(REG_ADC_15,  &adc15);
    es_read(REG_ADC_17,  &adc17);
    es_read(REG_DAC_31,  &dac31);
    es_read(REG_DAC_VOL, &dac_vol);
    ESP_LOGI(TAG, "ES8311 resumed — id=0x%02X CLK6=0x%02X CLK8=0x%02X SYS14=0x%02X ADC15=0x%02X ADC17=0x%02X DAC31=0x%02X DACVOL=0x%02X",
             chip_id, clk6, clk8, sys14, adc15, adc17, dac31, dac_vol);
    if (dac31 != 0x00) {
        ESP_LOGE(TAG, "DAC31 not cleared! DAC may be muted (expected 0x00, got 0x%02X)", dac31);
    }
    return ESP_OK;
}
