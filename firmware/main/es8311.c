/**
 * ES8311 Audio Codec — register init for 16kHz/16-bit/mono voice recording.
 *
 * Clock setup: ESP32 I2S master provides MCLK = 256 * 16kHz = 4.096 MHz.
 * ES8311 is I2S slave. BCLK divider N = MCLK / BCLK = 4096000 / 512000 = 8.
 *
 * Register values cross-referenced against:
 *   - ES8311 datasheet (Everset)
 *   - espressif/esp-adf es8311 driver (es8311_sample_frequency_config table)
 */

#include "es8311.h"
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
#define REG_CLK4     0x04  /* ADC BCLK divider low byte */
#define REG_CLK5     0x05
#define REG_CLK6     0x06  /* DAC BCLK divider low byte */
#define REG_CLK7     0x07  /* ADC/DAC OSR */
#define REG_CLK8     0x08  /* ADC OSR selector */
#define REG_SDPIN    0x09  /* I2S DAC input format */
#define REG_SDPOUT   0x0A  /* I2S ADC output format */
#define REG_SYS_D    0x0D  /* Power management */
#define REG_SYS_E    0x0E  /* Analog references */
#define REG_SYS_F    0x0F  /* REF/VSEL */
#define REG_SYS_10   0x10  /* Clock enables */
#define REG_SYS_11   0x11  /* HP reference */
#define REG_SYS_12   0x12  /* LDO */
#define REG_SYS_13   0x13  /* HP output power */
#define REG_SYS_14   0x14  /* Charge pump */
#define REG_ADC_15   0x15  /* ADC power, PGA, MICBIAS */
#define REG_ADC_16   0x16  /* ADC HPF, volume trim */
#define REG_ADC_17   0x17  /* ADC automute */
#define REG_ADC_18   0x18  /* ADC EQ */
#define REG_ADC_PGA  0x1B  /* MIC PGA gain */
#define REG_ADC_VOL  0x1C  /* ADC digital volume */
#define REG_DAC_31   0x31  /* DAC power */
#define REG_DAC_VOL  0x32  /* DAC digital volume */
#define REG_DAC_35   0x35  /* DAC ramp rate */
#define REG_DAC_37   0x37  /* DAC mono mix */
#define REG_CHIPID1  0xFD  /* Should read 0x83 */

static i2c_master_dev_handle_t dev_handle = NULL;

static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

static esp_err_t es_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, val, 1, 100);
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

    /* Clock manager — MCLK=4.096MHz from pin, BCLK div=8, OSR for 16kHz */
    es_write(REG_CLK1, 0x00);  /* MCLK from pin, pre-div=1 */
    es_write(REG_CLK2, 0x00);  /* MCLK integer div=1 (passthrough) */
    es_write(REG_CLK3, 0x00);  /* ADC single-speed, BCLK div MSBs=0 */
    es_write(REG_CLK4, 0x08);  /* ADC BCLK div N=8 → 512kHz */
    es_write(REG_CLK5, 0x00);  /* DAC single-speed */
    es_write(REG_CLK6, 0x08);  /* DAC BCLK div N=8 */
    es_write(REG_CLK7, 0x7F);  /* OSR config for 16kHz (from esp-adf table) */
    es_write(REG_CLK8, 0x00);  /* ADC OSR = 256x (best SNR) */

    /* I2S format: Philips standard, 16-bit — critical for correct audio capture */
    ESP_RETURN_ON_ERROR(es_write(REG_SDPIN,  0x0C), TAG, "I2S DAC format failed");
    ESP_RETURN_ON_ERROR(es_write(REG_SDPOUT, 0x0C), TAG, "I2S ADC format failed");

    /* System power-up — critical */
    ESP_RETURN_ON_ERROR(es_write(REG_SYS_D,  0x01), TAG, "Power-up failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    es_write(REG_SYS_E,  0x0A);  /* Bias on */
    es_write(REG_SYS_F,  0x00);  /* 3.3V reference */
    es_write(REG_SYS_10, 0x1C);  /* ADC+DAC clocks enabled */
    es_write(REG_SYS_11, 0x00);  /* Default */
    es_write(REG_SYS_12, 0x00);  /* LDO 1.35V */
    es_write(REG_SYS_13, 0x10);  /* HP off */
    es_write(REG_SYS_14, 0x1A);  /* Charge pump on */

    /* ADC path: power on, PGA on, MICBIAS on, single-ended mic */
    es_write(REG_ADC_15, 0x38);  /* 0xB8 had bit 7 set — ADC PDN (active-low power-down) */
    es_write(REG_ADC_16, 0x80);  /* HPF enabled (DC removal) */
    es_write(REG_ADC_17, 0x00);  /* Automute disabled */
    es_write(REG_ADC_18, 0x02);  /* EQ bypass (flat response) */
    es_write(REG_ADC_PGA, mic_gain);
    es_write(REG_ADC_VOL, 0x00); /* 0 dB digital volume */
    es_write(REG_DAC_37,  0x08); /* Normal DAC path */

    ESP_LOGI(TAG, "ES8311 initialized: 16kHz 16-bit, PGA=%ddB", mic_gain * 3);
    return ESP_OK;
}

esp_err_t es8311_enable_dac(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    es_write(REG_DAC_31, 0x00);  /* DAC power on (0x60 mutes DSM+DEM!) */
    es_write(REG_DAC_VOL, 0x00); /* 0 dB */
    es_write(REG_DAC_35, 0x20);  /* Moderate ramp (pop suppression) */
    es_write(REG_SYS_14, 0xBF);  /* Full charge pump */
    ESP_LOGI(TAG, "DAC enabled");
    return ESP_OK;
}

esp_err_t es8311_mute_dac(bool mute)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    return es_write(REG_DAC_VOL, mute ? 0xC0 : 0x00);
}

esp_err_t es8311_suspend(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(es_write(REG_SYS_14, 0x00), TAG, "charge pump suspend failed");
    ESP_RETURN_ON_ERROR(es_write(REG_ADC_15, 0xB8), TAG, "ADC suspend failed");
    ESP_RETURN_ON_ERROR(es_write(REG_DAC_31, 0x60), TAG, "DAC suspend failed");
    ESP_LOGI(TAG, "ES8311 suspended");
    return ESP_OK;
}

esp_err_t es8311_resume(void)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(es_write(REG_SYS_14, 0xBF), TAG, "charge pump resume failed");
    ESP_RETURN_ON_ERROR(es_write(REG_ADC_15, 0x38), TAG, "ADC resume failed");
    ESP_RETURN_ON_ERROR(es_write(REG_DAC_31, 0x00), TAG, "DAC resume failed");
    vTaskDelay(pdMS_TO_TICKS(10));  // Analog settling
    ESP_LOGI(TAG, "ES8311 resumed");
    return ESP_OK;
}
