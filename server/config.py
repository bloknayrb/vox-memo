"""Vox Memo server configuration."""

import os
from pathlib import Path

# LLM cleanup
CLEANUP_ENABLED = True
CLEANUP_MODEL = "gpt-4o-mini"

# Output
OBSIDIAN_INBOX = Path(os.environ.get("OBSIDIAN_INBOX", "~/Obsidian/vault/Inbox")).expanduser()
ARCHIVE_AUDIO = True  # keep WAV files alongside markdown

# Server
HOST = "0.0.0.0"
PORT = 8000

# Recording limits (sent to ESP32 on handshake)
MAX_MEMO_DURATION_SECONDS = 120
