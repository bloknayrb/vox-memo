#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NVS_NS = "vox_settings";

static app_settings_t g_settings = {
    .volume          = 0xEF,
    .brightness      = 0xFF,
    .accent_color    = 0x5C997C,
    .clock_24h       = true,
    .font_large      = false,
};

esp_err_t settings_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings — using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s) — using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    uint32_t u32;
    uint8_t  u8;

    if (nvs_get_u8 (h, "volume",     &u8)  == ESP_OK) g_settings.volume           = u8;
    if (nvs_get_u8 (h, "brightness", &u8)  == ESP_OK) g_settings.brightness       = u8;
    if (nvs_get_u32(h, "accent",     &u32) == ESP_OK) g_settings.accent_color     = u32;
    if (nvs_get_u8 (h, "clock24h",   &u8)  == ESP_OK) g_settings.clock_24h        = (bool)u8;
    if (nvs_get_u8 (h, "font_large", &u8)  == ESP_OK) g_settings.font_large       = (bool)u8;

    nvs_close(h);

    ESP_LOGI(TAG, "Settings loaded: vol=0x%02X bri=0x%02X accent=0x%06lX 24h=%d large=%d",
             g_settings.volume, g_settings.brightness,
             (unsigned long)g_settings.accent_color,
             g_settings.clock_24h, g_settings.font_large);
    return ESP_OK;
}

app_settings_t *settings_get(void) {
    return &g_settings;
}

void settings_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8 (h, "volume",     g_settings.volume);
    nvs_set_u8 (h, "brightness", g_settings.brightness);
    nvs_set_u32(h, "accent",     g_settings.accent_color);
    nvs_set_u8 (h, "clock24h",   (uint8_t)g_settings.clock_24h);
    nvs_set_u8 (h, "font_large", (uint8_t)g_settings.font_large);

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved");
}
