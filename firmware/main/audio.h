#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Audio configuration — ES8311 codec via I2S
// GPIO pins confirmed from ESP32-C6-Touch-LCD-1.83 (same platform)
#define AUDIO_I2S_NUM       I2S_NUM_0
#define AUDIO_SAMPLE_RATE   16000   // Whisper expects 16kHz
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1

// I2S GPIO pins (verified against Waveshare C6 reference schematic)
#define AUDIO_I2S_MCLK      GPIO_NUM_19
#define AUDIO_I2S_BCLK      GPIO_NUM_20
#define AUDIO_I2S_WS        GPIO_NUM_22   // LR clock
#define AUDIO_I2S_DOUT      GPIO_NUM_23   // Speaker output
#define AUDIO_I2S_DIN       GPIO_NUM_21   // Mic input

// I2C pins for ES8311 codec control
#define AUDIO_I2C_SDA       GPIO_NUM_7
#define AUDIO_I2C_SCL       GPIO_NUM_8

// Speaker amplifier enable (PA control) — set HIGH to enable
// GPIO6 matches C6-2.06 BSP (GPIO0 is QSPI PCLK for display)
#define AUDIO_PA_CTRL       GPIO_NUM_6

// DMA configuration — extra descriptors to absorb LittleFS write latency
#define AUDIO_DMA_DESC_NUM  14
#define AUDIO_DMA_BUF_LEN   1024

// Recording limits
#define AUDIO_MAX_DURATION_SEC  120

// Memo storage path on LittleFS
#define MEMO_BASE_PATH      "/memos"

/**
 * Initialize ES8311 codec and I2S peripheral.
 * Must be called after LittleFS is mounted.
 */
esp_err_t audio_init(void);

/**
 * Start recording from microphone to flash.
 * Creates a new WAV file at /memos/YYYYMMDD_HHMMSS.wav.
 * Recording runs in a FreeRTOS task until audio_stop_recording() is called
 * or MAX_DURATION is reached.
 */
esp_err_t audio_start_recording(void);

/**
 * Stop the current recording. Finalizes WAV header with actual byte count.
 */
esp_err_t audio_stop_recording(void);

/**
 * Returns true if currently recording.
 */
bool audio_is_recording(void);

/**
 * Play back a WAV file from flash through the speaker.
 * @param filename  Full path like "/memos/20260217_153000.wav"
 */
esp_err_t audio_play(const char *filename);

/**
 * Stop playback if active.
 */
esp_err_t audio_stop_playback(void);

/**
 * Get the number of memo files queued on flash.
 */
int audio_get_memo_count(void);

/**
 * Get elapsed recording time in seconds (0 if not recording).
 */
int audio_get_recording_elapsed(void);

/**
 * Get a list of memo filenames. Caller must free the returned array
 * and each string within it.
 * @param count  Output: number of files found
 * @return       Array of filename strings, or NULL on error
 */
char **audio_list_memos(int *count);

/**
 * Delete a memo file from flash.
 * @param filename  Full path like "/memos/20260217_153000.wav"
 */
esp_err_t audio_delete_memo(const char *filename);
