# Vox Memo

Press-to-talk voice capture device that transcribes and drops notes into Obsidian.

**Hardware:** Waveshare ESP32-C6-Touch-AMOLED-1.8

## How it works

1. **Hold button** → speak your thought → **release** — audio saves to flash
2. When on home Wi-Fi, device auto-syncs queued memos to PC backend
3. PC transcribes (faster-whisper) → Claude cleans up + tags → markdown note lands in `Inbox/`

Capture is fully offline. All intelligence runs on the PC.

## Project structure

```
firmware/       ESP32-C6 firmware (ESP-IDF + LVGL)
  main/
    main.c      Entry point, task spawning, main loop
    audio.c/h   I2S capture from ES8311, LittleFS storage
    display.c/h LVGL screens on SH8601 AMOLED
    touch.c/h   Button + touch input handling
    network.c/h Wi-Fi + HTTP upload to PC server
    imu.c/h     QMI8658 wake/sleep detection

server/         Python backend
  server.py     FastAPI — receives audio, transcribes, saves markdown
  config.py     Settings (whisper model, output path, etc.)
```

## Setup

### PC Backend

```bash
cd server
uv sync
# Set ANTHROPIC_API_KEY in environment for Claude cleanup
uv run python server.py
```

Server runs on port 8000. Endpoints:
- `POST /memo` — receive WAV audio, transcribe, save to Obsidian
- `GET /health` — health check (ESP32 uses this to detect server)

### ESP32 Firmware

Requires [ESP-IDF v5.3+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/) and [Waveshare BSP components](https://github.com/waveshareteam/Waveshare-ESP32-components).

```bash
# Clone Waveshare components
git clone https://github.com/waveshareteam/Waveshare-ESP32-components ~/GitHub/Waveshare-ESP32-components

# Build and flash
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

Before building, update:
- `network.c`: Wi-Fi SSID/password and server IP
- `audio.c`: Verify I2S GPIO pins against your board's schematic

## Implementation phases

- [x] Phase 1: ESP32 hello world — display, buttons, mic verify
- [ ] Phase 2: Record to flash — button hold/release, WAV files
- [ ] Phase 3: PC backend MVP — transcription pipeline
- [ ] Phase 4: Wi-Fi sync — auto-upload queued memos
- [ ] Phase 5: LVGL UI — idle/recording/queue/sync screens
- [ ] Phase 6: Polish — Claude cleanup, IMU, battery indicator
