# Vox Memo

## Build
- ESP-IDF framework, target ESP32-C6
- Build: `cd firmware && idf.py build` — Flash: `idf.py -p COMx flash monitor`
- **sdkconfig gotcha**: `sdkconfig.defaults` only apply when `sdkconfig` doesn't exist. Delete `sdkconfig` and rebuild after changing defaults.
- Server: `cd server && docker compose up -d --build` (requires `server/.env` with `OPENAI_API_KEY`)

## Hardware (Waveshare ESP32-C6 1.47" AMOLED)
- Display: SH8601 AMOLED 368x448, SPI (QSPI mode)
- Touch: FT3168 on I2C (GPIO7 SDA, GPIO8 SCL)
- Audio codec: ES8311 on same I2C bus, I2S for audio data
- PMIC: AXP2101 on same I2C bus — battery fuel gauge, charging
- Speaker amp: GPIO6 (AUDIO_PA_CTRL) — external PA enable pin
- Boot button: GPIO9 — used as recording trigger
- WiFi: 2.4GHz only (ESP32-C6 limitation)
- Audio format: 16kHz, 16-bit, mono (32,000 bytes/sec)

## Firmware Architecture (firmware/main/)
- `audio.c/h` — I2S record/playback, LittleFS memo storage, ES8311 codec control
- `display.c/h` — LVGL screens (idle, recording, queue, sync_confirm, settings), gestures, ambient clock mode
- `touch.c/h` — Touch polling, button press handling, gesture detection
- `es8311.c/h` — Low-level I2C register driver for ES8311 audio codec
- `network.c/h` — Multi-SSID WiFi STA, HTTP sync to server, settings-driven sync interval
- `settings.c/h` — NVS-backed user preferences (sync interval, volume, brightness, accent color, clock format, font size)
- `axp2101.c/h` — AXP2101 PMIC driver (battery SOC, VBUS detection, charging)
- `usb_sync.c/h` — USB Serial/JTAG memo transfer protocol (LIST/GET/DELETE/PING + CRC32)
- Memos stored in LittleFS at `/memos/YYYYMMDD_HHMMSS.wav`
- **Navigation**: Settings ←swipe→ Idle ←swipe→ Queue
- Boot button (GPIO9): short tap = sync, long press = record

## Server (server/)
- Python (uv), receives WAV → OpenAI Whisper transcription → gpt-4o-mini cleanup → saves .md to Obsidian inbox
- Dockerized with docker-compose; `.env` holds secrets (gitignored)
