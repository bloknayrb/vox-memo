# Vox Memo

Press-to-talk voice capture device that transcribes and drops notes into Obsidian.

**Hardware:** Waveshare ESP32-C6-Touch-AMOLED-1.47

## How it works

1. **Hold button** → speak your thought → **release** — audio saves to flash
2. When on home Wi-Fi, device auto-syncs queued memos to PC backend
3. PC transcribes (OpenAI Whisper API) → GPT-4o-mini cleans up + tags → markdown note lands in `Inbox/`

Capture is fully offline. All intelligence runs on the PC.

## Project structure

```
firmware/       ESP32-C6 firmware (ESP-IDF + LVGL)
  main/
    main.c        Entry point, task spawning, main loop
    audio.c/h     I2S capture from ES8311, LittleFS storage
    display.c/h   LVGL screens on SH8601 AMOLED
    touch.c/h     Button + touch input handling
    network.c/h   Wi-Fi + HTTP upload to PC server
    axp2101.c/h   PMIC — battery fuel gauge, charging status
    usb_sync.c/h  USB serial memo transfer (offline sync)

server/              Python backend
  server.py          FastAPI — receives audio, transcribes, saves markdown
  usb_sync.py        USB serial bridge (serial → HTTP, for offline sync)
  notify_watcher.py  Desktop toast notifications on new notes (Windows)
  config.py          Settings (paths, models, archive toggle)
  Dockerfile         Container build
  docker-compose.yml Compose config with Obsidian volume mount
  .env.example       Environment variable template
```

## Setup

### PC Backend

**Primary (Docker):**

```bash
cp server/.env.example server/.env
# Fill in OPENAI_API_KEY and OBSIDIAN_INBOX_HOST (path to your Obsidian inbox)
cd server
docker compose up -d --build
```

**Alternative (bare Python):**

```bash
cd server
uv sync
export OPENAI_API_KEY=sk-...
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

# Configure credentials
cp firmware/main/secrets.h.example firmware/main/secrets.h
# Edit secrets.h: fill in SSID, password, server IP
# Optionally uncomment the hotspot block for multi-network fallback

# Build and flash
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

## Features

- **Idle screen** — clock, battery %, Wi-Fi SSID (shows active network name), queued memo count
- **Multi-network fallback** — configure home Wi-Fi + hotspot in `secrets.h`; device cycles automatically
- **Storage-aware recording** — max duration shrinks as flash fills up (capped at 120s)
- **USB sync** — transfer memos over USB serial when Wi-Fi isn't available (`server/usb_sync.py`)
- **Desktop notifications** — toast alert on new Obsidian note (`server/notify_watcher.py`)
- **Auto-discard** — recordings under 1s are silently dropped (accidental taps)

## Status

Fully functional.

## Troubleshooting / Bug Fixes

- **Periodic White Flash / Reboot Loop:** The device would occasionally crash and reboot (showing a white flash on startup) when entering light sleep. This was caused by the LVGL touch driver polling the I2C bus while it was powered down by the system. This was fixed by:
  - Adding `esp_pm_lock`s to keep the display and APB bus awake while active.
  - Removing a fatal `ESP_ERROR_CHECK` in the `espressif__esp_lvgl_port` touch driver so occasional I2C glitches during sleep power transitions don't crash the entire system.
  - Disabling auto light sleep while connected to USB (`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` in `sdkconfig.defaults`) so the Serial/JTAG console doesn't disconnect.
- **Clock Stuck at 00:00:** The SNTP time sync failed to start because it attempted to use 2 time servers but the lwIP networking stack was only configured for 1. Fixed by setting `CONFIG_LWIP_SNTP_MAX_SERVERS=2` in `sdkconfig.defaults` and explicitly setting `.start = true` in the `esp_sntp_config_t` struct in `network.c`.
- **Font Size Button Unresponsive on Settings Screen:** Toggling the font size updated the rest of the UI but not the settings screen itself. This was because the text labels on the settings screen were created as local variables and couldn't be referenced when the theme updated. Fixed by promoting the labels to global variables and explicitly applying the new font sizing to them in `display_apply_theme()`.
