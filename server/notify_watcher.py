"""Watch Obsidian inbox for new vox-memo transcriptions and send Windows toast notifications."""

import os
import re
import time
from pathlib import Path

from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer
from winotify import Notification

INBOX = Path(os.environ.get("OBSIDIAN_INBOX", "~/Obsidian/vault/Inbox")).expanduser()
DEBOUNCE_SEC = 2.0
MAX_PREVIEW = 120  # chars of body to show in toast


class MemoHandler(FileSystemEventHandler):
    def __init__(self):
        self._seen: dict[str, float] = {}

    def on_created(self, event):
        if event.is_directory:
            return
        path = Path(event.src_path)
        if path.suffix.lower() != ".md":
            return

        # Debounce — Docker WSL2 bind mounts can fire duplicates
        now = time.monotonic()
        if path.name in self._seen and (now - self._seen[path.name]) < DEBOUNCE_SEC:
            return
        self._seen[path.name] = now

        # Read with retry — file may not be fully flushed yet
        content = self._read_with_retry(path)
        if content is None:
            return

        # Only notify for vox-memo files (check frontmatter)
        if "source: vox-memo" not in content[:500]:
            return

        title = path.stem
        preview = self._extract_preview(content)
        self._send_toast(title, preview)

    def _read_with_retry(self, path: Path, retries: int = 10, delay: float = 0.2) -> str | None:
        for attempt in range(retries):
            try:
                text = path.read_text(encoding="utf-8")
                if text.strip():
                    return text
            except (OSError, PermissionError):
                pass
            time.sleep(delay)
        return None

    def _extract_preview(self, content: str) -> str:
        # Strip YAML frontmatter
        body = re.sub(r"^---.*?---\s*", "", content, count=1, flags=re.DOTALL)
        body = body.strip()
        if len(body) > MAX_PREVIEW:
            body = body[:MAX_PREVIEW] + "..."
        return body or "(empty)"

    def _send_toast(self, title: str, body: str):
        toast = Notification(
            app_id="Vox Memo",
            title=title,
            msg=body,
        )
        toast.show()
        print(f"[toast] {title}: {body[:60]}")


def main():
    if not INBOX.is_dir():
        print(f"Inbox not found: {INBOX}")
        print("Set OBSIDIAN_INBOX env var to your Obsidian inbox path.")
        return

    print(f"Watching {INBOX} for new vox-memo transcriptions...")
    handler = MemoHandler()
    observer = Observer()
    observer.schedule(handler, str(INBOX), recursive=False)
    observer.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        observer.stop()
    observer.join()


if __name__ == "__main__":
    main()
