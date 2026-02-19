#include "audio.h"
#include "display.h"
#include "es8311.h"
#include "network.h"

#include <dirent.h>
#include <math.h>
#include "esp_littlefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* GPIO6 = PA_CTRL — direct speaker amplifier enable (active high) */
#define AUDIO_PA_CTRL_GPIO  GPIO_NUM_6

static void pa_enable(bool on)
{
    gpio_set_level(AUDIO_PA_CTRL_GPIO, on ? 1 : 0);
}

/* XCA9554 I2C I/O expander — controls speaker PA enable on pin 7 */
#define XCA9554_I2C_ADDR  0x20
static i2c_master_dev_handle_t xca9554_dev = NULL;

static esp_err_t xca9554_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XCA9554_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &xca9554_dev);
    if (ret != ESP_OK) return ret;

    uint8_t cfg[2] = {0x03, 0x7F};  /* Config reg: pin 7 = output, others = input */
    ret = i2c_master_transmit(xca9554_dev, cfg, 2, 100);
    if (ret != ESP_OK) return ret;

    uint8_t out[2] = {0x01, 0x80};  /* Output reg: pin 7 HIGH (Waveshare ref keeps it on always) */
    return i2c_master_transmit(xca9554_dev, out, 2, 100);
}

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
static volatile uint32_t playback_bytes_played = 0;
static volatile uint32_t playback_bytes_total = 0;
static FILE *rec_file = NULL;
static int64_t rec_start_us = 0;
static uint32_t rec_bytes_written = 0;
static char rec_filepath[64] = {0};
static volatile bool last_rec_discarded = false;
static int rec_max_sec = AUDIO_MAX_DURATION_SEC;
static volatile int rec_rms = 0;
static volatile bool rec_storage_full = false;

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
    /* Stereo I2S buffer: 2 bytes/sample × 2 channels × N frames.
     * dma_frame_num = 512, so each read = 512 frames × 4 bytes = 2048 bytes.
     * We extract the left channel (every other int16) → 1024 bytes written per cycle. */
    uint8_t stereo_buf[AUDIO_DMA_BUF_LEN * 2];
    int16_t mono_buf[AUDIO_DMA_BUF_LEN / 2];  /* 512 mono samples = 1024 bytes */
    size_t bytes_read = 0;

    while (recording) {
        // Check max duration
        int elapsed = (int)((esp_timer_get_time() - rec_start_us) / 1000000);
        if (elapsed >= rec_max_sec) {
            ESP_LOGW(TAG, "Max duration reached (%ds), stopping", rec_max_sec);
            recording = false;
            break;
        }

        esp_err_t ret = i2s_channel_read(rx_chan, stereo_buf, sizeof(stereo_buf), &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read error: %s (bytes_read=%u)", esp_err_to_name(ret), (unsigned)bytes_read);
        }
        if (ret == ESP_OK && bytes_read > 0 && rec_file) {
            /* One-shot: log first 32 raw bytes to confirm I2S data is non-zero */
            static int dump_count = 0;
            if (dump_count < 3) {
                ESP_LOG_BUFFER_HEXDUMP("audio_raw", stereo_buf, 64, ESP_LOG_DEBUG);
                dump_count++;
            }
            /* Extract left channel: stereo frame = [L int16, R int16], take every L */
            const int16_t *stereo = (const int16_t *)stereo_buf;
            int frames = bytes_read / 4;  /* 4 bytes per stereo frame */
            for (int i = 0; i < frames; i++) {
                mono_buf[i] = stereo[i * 2];  /* left channel */
            }
            // Calculate RMS for VU meter
            int64_t sum_sq = 0;
            for (int i = 0; i < frames; i++) {
                int32_t s = mono_buf[i];
                sum_sq += s * s;
            }
            rec_rms = (int)sqrtf((float)sum_sq / frames);

            size_t mono_bytes = (size_t)frames * 2;
            if (fwrite(mono_buf, 1, mono_bytes, rec_file) != mono_bytes) {
                ESP_LOGE(TAG, "fwrite failed — disk full? Stopping recording");
                rec_storage_full = true;
                recording = false;
                break;
            }
            rec_bytes_written += mono_bytes;
        }
    }

    // Finalize: discard if too short, otherwise write WAV header
    if (rec_file) {
        if (rec_bytes_written < AUDIO_MIN_DURATION_BYTES) {
            fclose(rec_file);
            rec_file = NULL;
            remove(rec_filepath);
            last_rec_discarded = true;
            ESP_LOGW(TAG, "Recording discarded (too short): %s (%lu bytes)",
                     rec_filepath, (unsigned long)rec_bytes_written);
        } else {
            write_wav_header(rec_file, rec_bytes_written);
            fclose(rec_file);
            rec_file = NULL;
            last_rec_discarded = false;
            ESP_LOGI(TAG, "Recording saved: %s (%lu bytes)",
                     rec_filepath, (unsigned long)rec_bytes_written);
        }
    }

    i2s_channel_disable(rx_chan);
    i2s_channel_disable(tx_chan);
    es8311_suspend();
    if (!last_rec_discarded) {
        network_wake();
    }
    xSemaphoreGive(rec_done_sem);
    vTaskDelete(NULL);
}

esp_err_t audio_init(void) {
    ESP_LOGI(TAG, "Audio init — configure I2S + ES8311 codec");

    rec_done_sem = xSemaphoreCreateBinary();
    play_done_sem = xSemaphoreCreateBinary();

    /* GPIO21 (I2S DIN / codec SDOUT) pre-I2S level check.
     * Pull-up then read: if HIGH → line is floating (codec SDOUT tri-stated).
     * If LOW with pull-up → codec is actively driving SDOUT low.
     * Either way, confirms whether the hardware path is alive. */
    gpio_set_direction(AUDIO_I2S_DIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(AUDIO_I2S_DIN, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(1));
    int din_level = gpio_get_level(AUDIO_I2S_DIN);
    ESP_LOGI(TAG, "GPIO%d (DIN) pre-I2S level with pull-up: %s",
             AUDIO_I2S_DIN, din_level ? "HIGH (floating/no signal)" : "LOW (actively driven)");
    gpio_set_pull_mode(AUDIO_I2S_DIN, GPIO_FLOATING);

    // Configure PA GPIO — keep low (off) until playback starts
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    pa_enable(false);

    // Configure I2S standard mode for both TX (speaker) and RX (mic).
    // STEREO mode matches the ES8311 hardware output — recording extracts left channel only.
    // ESP32 is I2S master: drives MCLK (4.096 MHz), BCLK (512kHz), WS (16kHz) to ES8311 slave.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN / (AUDIO_BITS / 8);  // frames per DMA buffer

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // MCLK = 256 * Fs = 4.096 MHz
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,   // GPIO19
            .bclk = AUDIO_I2S_BCLK,   // GPIO20
            .ws   = AUDIO_I2S_WS,     // GPIO22
            .dout = AUDIO_I2S_DOUT,   // GPIO23 — ESP32 TX → ES8311 SDIN → speaker
            .din  = AUDIO_I2S_DIN,    // GPIO21 — ES8311 SDOUT → ESP32 RX → mic
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    // Initialize ES8311 codec and XCA9554 PA expander via shared I2C bus
    i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)display_get_i2c_handle();
    if (i2c_bus) {
        esp_err_t codec_ret = es8311_init(i2c_bus, ES8311_MIC_GAIN_24DB);
        if (codec_ret == ESP_OK) {
            es8311_enable_dac();  // Enable DAC path for future playback
            es8311_suspend();     // Start asleep — resume on record/play
            ESP_LOGI(TAG, "ES8311 codec initialized");
        } else {
            ESP_LOGW(TAG, "ES8311 init failed (%s) — audio may not work",
                     esp_err_to_name(codec_ret));
        }

        esp_err_t pa_ret = xca9554_init(i2c_bus);
        if (pa_ret != ESP_OK) {
            ESP_LOGW(TAG, "XCA9554 PA expander init failed (%s) — speaker may not work",
                     esp_err_to_name(pa_ret));
        } else {
            ESP_LOGI(TAG, "XCA9554 PA expander initialized");
        }
    } else {
        ESP_LOGW(TAG, "No I2C bus available — codec and PA not initialized");
    }

    // Create memos directory
    mkdir(MEMO_BASE_PATH, 0755);

    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

esp_err_t audio_start_recording(void) {
    if (recording || playing) {
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

    size_t lfs_total = 0, lfs_used = 0;
    esp_littlefs_info("storage", &lfs_total, &lfs_used);
    size_t free_bytes = (lfs_used < lfs_total) ? (lfs_total - lfs_used) : 0;
    int storage_sec = (int)(free_bytes / AUDIO_BYTES_PER_SEC);
    rec_max_sec = (storage_sec < AUDIO_MAX_DURATION_SEC) ? storage_sec : AUDIO_MAX_DURATION_SEC;

    rec_file = fopen(rec_filepath, "wb");
    if (!rec_file) {
        ESP_LOGE(TAG, "Failed to open %s for writing", rec_filepath);
        return ESP_FAIL;
    }

    // Write placeholder WAV header (will be updated on stop)
    rec_bytes_written = 0;
    rec_storage_full = false;
    write_wav_header(rec_file, 0);

    esp_err_t codec_err = es8311_resume();
    if (codec_err != ESP_OK) {
        ESP_LOGE(TAG, "Codec resume failed, aborting recording");
        fclose(rec_file);
        rec_file = NULL;
        remove(rec_filepath);
        return codec_err;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
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

esp_err_t audio_stop_recording(bool *was_discarded) {
    if (!recording) {
        // Max-duration auto-stop may have already set recording=false.
        // Try to consume the semaphore in case the task just finished.
        if (xSemaphoreTake(rec_done_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (was_discarded) *was_discarded = last_rec_discarded;
            ESP_LOGI(TAG, "Recording already stopped%s", last_rec_discarded ? " (discarded)" : "");
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }
    recording = false;
    // Wait for task to finalize WAV header and close file before returning
    if (xSemaphoreTake(rec_done_sem, pdMS_TO_TICKS(5000)) == pdFALSE) {
        ESP_LOGW(TAG, "Semaphore timeout waiting for recording task to finish");
    }
    if (was_discarded) {
        *was_discarded = last_rec_discarded;
    }
    ESP_LOGI(TAG, "Recording stopped%s", last_rec_discarded ? " (discarded)" : "");
    return ESP_OK;
}

bool audio_is_recording(void) {
    return recording;
}

int audio_get_recording_elapsed(void) {
    if (!recording) return 0;
    return (int)((esp_timer_get_time() - rec_start_us) / 1000000);
}

int audio_get_recording_max_sec(void) {
    return rec_max_sec;
}

static void playback_task(void *arg) {
    /* WAV file is mono; I2S is stereo — duplicate each sample into L+R channels.
     * Read 1024 mono bytes → expand to 2048 stereo bytes per DMA cycle. */
    uint8_t mono_buf[AUDIO_DMA_BUF_LEN];
    uint8_t stereo_buf[AUDIO_DMA_BUF_LEN * 2];
    size_t bytes_written = 0;

    FILE *f = fopen(playback_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for playback", playback_path);
        playing = false;
        display_memo_playback_done();
        xSemaphoreGive(play_done_sem);
        vTaskDelete(NULL);
        return;
    }

    // Get total audio data size and skip WAV header
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    playback_bytes_total = (fsize > (long)sizeof(wav_header_t)) ? (uint32_t)(fsize - sizeof(wav_header_t)) : 0;
    playback_bytes_played = 0;
    fseek(f, sizeof(wav_header_t), SEEK_SET);

    ESP_LOGI(TAG, "Playback file opened: %s, total=%lu bytes", playback_path, (unsigned long)playback_bytes_total);

    // Quick RMS check on first chunk to verify recording has content
    {
        size_t n = fread(mono_buf, 1, sizeof(mono_buf), f);
        if (n > 0) {
            int64_t sum = 0;
            const int16_t *s = (const int16_t *)mono_buf;
            for (size_t i = 0; i < n / 2; i++) sum += (int64_t)s[i] * s[i];
            int rms = (int)sqrtf((float)(sum / (n / 2)));
            ESP_LOGI(TAG, "First chunk RMS: %d (0=silent, >500=has content)", rms);
        }
        fseek(f, sizeof(wav_header_t), SEEK_SET);  // rewind to start of audio
    }

    bool first_chunk = true;
    while (playing && !stop_playback) {
        size_t bytes_read = fread(mono_buf, 1, sizeof(mono_buf), f);
        if (bytes_read == 0) break;  // EOF
        playback_bytes_played += bytes_read;

        /* Expand mono → stereo: duplicate each int16 sample into L and R */
        const int16_t *mono = (const int16_t *)mono_buf;
        int16_t *stereo = (int16_t *)stereo_buf;
        int samples = bytes_read / 2;
        for (int i = 0; i < samples; i++) {
            stereo[i * 2]     = mono[i];  /* left */
            stereo[i * 2 + 1] = mono[i];  /* right */
        }
        size_t stereo_bytes = (size_t)samples * 4;

        esp_err_t ret = i2s_channel_write(tx_chan, stereo_buf, stereo_bytes, &bytes_written, pdMS_TO_TICKS(200));
        if (first_chunk) {
            ESP_LOGI(TAG, "First I2S write: ret=%s, written=%u", esp_err_to_name(ret), (unsigned)bytes_written);
            first_chunk = false;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(ret));
            break;
        }
    }

    fclose(f);

    /* Write one buffer of silence to flush DMA pipeline before disabling */
    memset(stereo_buf, 0, sizeof(stereo_buf));
    i2s_channel_write(tx_chan, stereo_buf, sizeof(stereo_buf), &bytes_written, pdMS_TO_TICKS(500));

    i2s_channel_disable(rx_chan);
    i2s_channel_disable(tx_chan);
    pa_enable(false);
    es8311_mute_dac(true);
    es8311_suspend();
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

    // Resume codec, unmute DAC and enable speaker amplifier with settling time
    esp_err_t codec_err = es8311_resume();
    if (codec_err != ESP_OK) {
        ESP_LOGE(TAG, "Codec resume failed, aborting playback");
        return codec_err;
    }
    es8311_mute_dac(false);
    pa_enable(true);
    vTaskDelay(pdMS_TO_TICKS(20));  // DAC + PA settling time
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));  // duplex: both channels share clock

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
    if (xSemaphoreTake(play_done_sem, pdMS_TO_TICKS(5000)) == pdFALSE) {
        ESP_LOGW(TAG, "Semaphore timeout waiting for playback task to finish");
    }
    return ESP_OK;
}

bool audio_is_playing(void) {
    return playing;
}

bool audio_get_playback_progress(int *elapsed_sec, int *total_sec) {
    if (!playing) return false;
    if (elapsed_sec) *elapsed_sec = (int)(playback_bytes_played / AUDIO_BYTES_PER_SEC);
    if (total_sec) *total_sec = (int)(playback_bytes_total / AUDIO_BYTES_PER_SEC);
    return true;
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

static int memo_path_cmp_newest_first(const void *a, const void *b) {
    return strcmp(*(const char **)b, *(const char **)a);
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
    qsort(list, (size_t)i, sizeof(char *), memo_path_cmp_newest_first);
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

bool audio_was_storage_full(void) {
    return rec_storage_full;
}

int audio_get_recording_rms(void) {
    return recording ? rec_rms : 0;
}
