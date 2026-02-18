/**
 * AXP2101 PMIC — battery SOC and VBUS detection.
 *
 * Register references from AXP2101 datasheet:
 *   0x00: Power status 0 (bit 5 = VBUS good)
 *   0x01: Power status 1 (bit 5 = VBUS present)
 *   0xA4: Battery SOC (0-100%)
 *   0x68: ADC enable (bit 7 = battery fuel gauge enable)
 */

#include "axp2101.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

/* Key registers */
#define REG_STATUS1     0x00
#define REG_STATUS2     0x01
#define REG_ADC_ENABLE  0x30
#define REG_BAT_SOC     0xA4

static i2c_master_dev_handle_t dev_handle = NULL;

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, val, 1, 100);
}

esp_err_t axp2101_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify chip is present by reading status register
    uint8_t status = 0;
    ret = axp_read(REG_STATUS1, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding");
        return ret;
    }

    // Enable fuel gauge (ADC for battery SOC)
    uint8_t adc_en = 0;
    axp_read(REG_ADC_ENABLE, &adc_en);
    adc_en |= 0x01;  // Enable battery gauge
    axp_write(REG_ADC_ENABLE, adc_en);

    ESP_LOGI(TAG, "AXP2101 initialized (status=0x%02X)", status);
    return ESP_OK;
}

int axp2101_get_battery_percent(void)
{
    if (!dev_handle) return -1;

    uint8_t soc = 0;
    esp_err_t ret = axp_read(REG_BAT_SOC, &soc);
    if (ret != ESP_OK) return -1;

    // SOC register returns 0-100; values > 100 indicate no battery
    if (soc > 100) return -1;
    return (int)soc;
}

bool axp2101_is_vbus_present(void)
{
    if (!dev_handle) return false;

    uint8_t status = 0;
    esp_err_t ret = axp_read(REG_STATUS1, &status);
    if (ret != ESP_OK) return false;

    return (status & (1 << 5)) != 0;  // Bit 5: VBUS good
}
