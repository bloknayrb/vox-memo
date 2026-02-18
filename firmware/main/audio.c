#include "audio.h"
#include "display.h"
#include "es8311.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// I2S channel handle
static i2s_chan_handle_t rx_chan = NULL;
static i2s_chan_handle_t tx_chan = NULL;

// Completion semaphores — given by task on exit, taken by stop functions to sync shutdown
static SemaphoreHandle_t rec_done_sem = NULL;
static SemaphoreHandle_t play_done_sem = NULL;

// Recording state
static volatile bool recording = false;
static volatile bool playing = false;
static volatile bool stop_playback = false;
static char playback_path[280] = {0};
static FILE *rec_file = NULL;
static int64_t rec_start_us = 0;
static uint32_t rec_bytes_written = 0;
static char rec_filepath[64] = {0};

// WAV header for 16kHz 16-bit mono PCM
typedef struct __attribute__((packed)) {
    char riff[4];           // "RIFF"
    uint32_t file_size;     // file size - 8
    char wave[4];           // "WAVE"
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // 16
    uint16_t audio_format;  // 1 (PCM)
    uint16_t channels;      // 1
    uint32_t sample_rate;   // 16000
    uint32_t byte_rate;     // 32000
    uint16_t block_align;   // 2
    uint16_t bits_per_sample; // 16
    char data[4];           // "data"
    uint32_t data_size;     // actual audio data size
} wav_header_t;

static void write_wav_header(FILE *f, uint32_t data_size) {
    wav_header_t hdr = {
        .riff = "RIFF",
        .file_size = data_size + sizeof(wav_header_t) - 8,
        .wave = "WAVE",
        .fmt = "fmt ",
        .fmt_size = 16,
        .audio_format = 1,
        .channels = AUDIO_CHANNELS,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8),
        .block_align = AUDIO_CHANNELS * (AUDIO_BITS / 8),
        .bits_per_sample = AUDIO_BITS,
        .data = "data",
        .data_size = data_size,
    };
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);
}

static void recording_task(void *arg) {
    uint8_t buf[AUDIO_DMA_BUF_LEN];
    size_t bytes_read = 0;

    while (recording) {
        // Check max duration
        int elapsed = (int)((esp_timer_get_time() - rec_start_us) / 1000000);
        if (elapsed >= AUDIO_MAX_DURATION_SEC) {
            ESP_LOGW(TAG, "Max duration reached (%ds), stopping", AUDIO_MAX_DURATION_SEC);
            recording = false;
            break;
        }

        esp_err_t ret = i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100));
        if (ret == ESP_OK && bytes_read > 0 && rec_file) {
            if (fwrite(buf, 1, bytes_read, rec_file) != bytes_read) {
                ESP_LOGE(TAG, "fwrite failed — disk full? Stopping recording");
                recording = false;
                break;
            }
            rec_bytes_written += bytes_read;
        }
    }

    // Finalize WAV header with actual size
    if (rec_file) {
        write_wav_header(rec_file, rec_bytes_written);
        fclose(rec_file);
        rec_file = NULL;
        ESP_LOGI(TAG, "Recording saved: %s (%lu bytes)", rec_filepath, (unsigned long)rec_bytes_written);
    }

    i2s_channel_disable(rx_chan);
    xSemaphoreGive(rec_done_sem);
    vTaskDelete(NULL);
}

esp_err_t audio_init(void) {
    ESP_LOGI(TAG, "Audio init — configure I2S + ES8311 codec");

    rec_done_sem = xSemaphoreCreateBinary();
    play_done_sem = xSemaphoreCreateBinary();

    // Enable PA (speaker amplifier) — active HIGH, off during init
    gpio_reset_pin(AUDIO_PA_CTRL);
    gpio_set_direction(AUDIO_PA_CTRL, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PA_CTRL, 0);

    // Configure I2S standard mode for both TX (speaker) and RX (mic)
    // MCLK = 256 * 16kHz = 4.096 MHz — required by ES8311 clock config
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN / (AUDIO_BITS / 8);

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // MCLK = 256 * Fs = 4.096 MHz
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,   // GPIO19
            .bclk = AUDIO_I2S_BCLK,   // GPIO20
            .ws   = AUDIO_I2S_WS,     // GPIO22
            .dout = AUDIO_I2S_DOUT,   // GPIO23 — to speaker
            .din  = AUDIO_I2S_DIN,    // GPIO21 — from mic
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    // Initialize ES8311 codec via I2C (bus shared with touch, created in display.c)
    i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)display_get_i2c_handle();
    if (i2c_bus) {
        esp_err_t codec_ret = es8311_init(i2c_bus, ES8311_MIC_GAIN_24DB);
        if (codec_ret == ESP_OK) {
            es8311_enable_dac();  // Enable DAC path for future playback
            ESP_LOGI(TAG, "ES8311 codec initialized");
        } else {
            ESP_LOGW(TAG, "ES8311 init failed (%s) — audio may not work",
                     esp_err_to_name(codec_ret));
        }
    } else {
        ESP_LOGW(TAG, "No I2C bus available — ES8311 codec not initialized");
    }

    // Create memos directory
    mkdir(MEMO_BASE_PATH, 0755);

    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

esp_err_t audio_start_recording(void) {
    if (recording) {
        return ESP_ERR_INVALID_STATE;
    }

    // Generate filename from current time
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(rec_filepath, sizeof(rec_filepath), MEMO_BASE_PATH "/%04d%02d%02d_%02d%02d%02d.wav",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    rec_file = fopen(rec_filepath, "wb");
    if (!rec_file) {
        ESP_LOGE(TAG, "Failed to open %s for writing", rec_filepath);
        return ESP_FAIL;
    }

    // Write placeholder WAV header (will be updated on stop)
    rec_bytes_written = 0;
    write_wav_header(rec_file, 0);

    // Enable I2S receive
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    recording = true;
    rec_start_us = esp_timer_get_time();

    // Drain any stale semaphore signal before starting
    xSemaphoreTake(rec_done_sem, 0);

    // Spawn recording task
    xTaskCreate(recording_task, "rec_task", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "Recording started: %s", rec_filepath);
    return ESP_OK;
}

esp_err_t audio_stop_recording(void) {
    if (!recording) {
        return ESP_ERR_INVALID_STATE;
    }
    recording = false;
    // Wait for task to finalize WAV header and close file before returning
    xSemaphoreTake(rec_done_sem, pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Recording stopped");
    return ESP_OK;
}

bool audio_is_recording(void) {
    return recording;
}

int audio_get_recording_elapsed(void) {
    if (!recording) return 0;
    return (int)((esp_timer_get_time() - rec_start_us) / 1000000);
}

static void playback_task(void *arg) {
    uint8_t buf[AUDIO_DMA_BUF_LEN];
    size_t bytes_written = 0;

    FILE *f = fopen(playback_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for playback", playback_path);
        playing = false;
        gpio_set_level(AUDIO_PA_CTRL, 0);
        display_memo_playback_done();
        xSemaphoreGive(play_done_sem);
        vTaskDelete(NULL);
        return;
    }

    // Skip WAV header (44 bytes)
    fseek(f, sizeof(wav_header_t), SEEK_SET);

    ESP_LOGI(TAG, "Playback started: %s", playback_path);

    while (playing && !stop_playback) {
        size_t bytes_read = fread(buf, 1, sizeof(buf), f);
        if (bytes_read == 0) break;  // EOF

        esp_err_t ret = i2s_channel_write(tx_chan, buf, bytes_read, &bytes_written, pdMS_TO_TICKS(200));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(ret));
            break;
        }
    }

    fclose(f);
    i2s_channel_disable(tx_chan);
    gpio_set_level(AUDIO_PA_CTRL, 0);
    playing = false;
    stop_playback = false;

    ESP_LOGI(TAG, "Playback finished: %s", playback_path);
    display_memo_playback_done();
    xSemaphoreGive(play_done_sem);
    vTaskDelete(NULL);
}

esp_err_t audio_play(const char *filename) {
    if (playing || recording) {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(playback_path, filename, sizeof(playback_path) - 1);
    playback_path[sizeof(playback_path) - 1] = '\0';

    // Enable speaker amplifier
    gpio_set_level(AUDIO_PA_CTRL, 1);

    // Enable I2S transmit channel
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // Drain any stale semaphore signal before starting
    xSemaphoreTake(play_done_sem, 0);

    playing = true;
    stop_playback = false;

    xTaskCreate(playback_task, "play_task", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "Playback requested: %s", filename);
    return ESP_OK;
}

esp_err_t audio_stop_playback(void) {
    if (!playing) return ESP_ERR_INVALID_STATE;
    stop_playback = true;
    // Wait for task to disable I2S and release hardware before returning
    xSemaphoreTake(play_done_sem, pdMS_TO_TICKS(5000));
    return ESP_OK;
}

int audio_get_memo_count(void) {
    int count = 0;
    DIR *dir = opendir(MEMO_BASE_PATH);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".wav") == 0) count++;
    }
    closedir(dir);
    return count;
}

char **audio_list_memos(int *count) {
    *count = 0;
    DIR *dir = opendir(MEMO_BASE_PATH);
    if (!dir) return NULL;

    // First pass: count
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".wav") == 0) n++;
    }

    if (n == 0) {
        closedir(dir);
        return NULL;
    }

    char **list = malloc(n * sizeof(char *));
    rewinddir(dir);

    int i = 0;
    while ((entry = readdir(dir)) != NULL && i < n) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".wav") == 0) {
            char path[280];  // MEMO_BASE_PATH(7) + "/" + NAME_MAX(255) + NUL
            snprintf(path, sizeof(path), MEMO_BASE_PATH "/%s", entry->d_name);
            list[i] = strdup(path);
            i++;
        }
    }
    closedir(dir);
    *count = i;
    return list;
}

esp_err_t audio_delete_memo(const char *filename) {
    if (remove(filename) == 0) {
        ESP_LOGI(TAG, "Deleted: %s", filename);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to delete: %s", filename);
    return ESP_FAIL;
}
