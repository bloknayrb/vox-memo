"""Vox Memo — FastAPI server that receives audio from ESP32, transcribes, and saves to Obsidian."""

import array
import json
import math
import os
import tempfile
import wave
from datetime import datetime, timezone
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request, Response
from openai import OpenAI, OpenAIError

import config

app = FastAPI(title="Vox Memo")

# Lazy-loaded globals
_openai_client: OpenAI | None = None


def get_openai() -> OpenAI:
    global _openai_client
    if _openai_client is None:
        _openai_client = OpenAI()
    return _openai_client


SILENCE_RMS_THRESHOLD = 400  # 16kHz/16-bit mono; speech typically 1000–8000 RMS


def transcribe(audio_path: Path) -> str | None:
    """Transcribe WAV file using OpenAI Whisper API. Returns None if audio is silent.
    Raises OpenAIError on failure."""
    with wave.open(str(audio_path)) as wf:
        pcm = wf.readframes(wf.getnframes())
        samples = array.array('h', pcm)
        rms = math.sqrt(sum(s*s for s in samples) / len(samples)) if samples else 0
        duration_s = wf.getnframes() / wf.getframerate()
    print(f"[audio] {duration_s:.1f}s  RMS={rms:.0f}  bytes={len(pcm)}")

    if rms < SILENCE_RMS_THRESHOLD:
        print(f"[audio] Silence gate — RMS {rms:.0f} below threshold, skipping Whisper")
        return None

    client = get_openai()
    with open(audio_path, "rb") as f:
        result = client.audio.transcriptions.create(model="whisper-1", file=f, language="en")
    print(f"Transcribed: {result.text[:80]}...")
    return result.text


def cleanup(raw_text: str) -> dict | None:
    """Send raw transcription to OpenAI for cleanup. Returns {title, content, tags} or None on failure."""
    if not config.CLEANUP_ENABLED:
        return None
    try:
        client = get_openai()
        response = client.chat.completions.create(
            model=config.CLEANUP_MODEL,
            response_format={"type": "json_object"},
            messages=[{
                "role": "user",
                "content": (
                    "Clean up this voice memo transcription for an Obsidian vault. "
                    "Fix grammar, remove filler words (um, uh, like, you know). "
                    "Preserve the speaker's meaning and tone. "
                    "Suggest a concise title and 1-3 tags.\n"
                    "Return ONLY valid JSON: {\"title\": \"...\", \"content\": \"...\", \"tags\": [\"tag1\", \"tag2\"]}\n"
                    "Tags should be lowercase, no spaces (use-dashes).\n\n"
                    f"Transcription:\n{raw_text}"
                ),
            }],
        )
        result_text = response.choices[0].message.content.strip()
        return json.loads(result_text)
    except (OpenAIError, json.JSONDecodeError, KeyError) as e:
        print(f"Cleanup failed: {e}")
        return None


def write_memo(timestamp: datetime, title: str, content: str, tags: list[str]) -> Path:
    """Write markdown note to Obsidian Inbox."""
    config.OBSIDIAN_INBOX.mkdir(parents=True, exist_ok=True)

    # Build frontmatter
    tag_lines = "\n".join(f"  - {t}" for t in tags) if tags else "  - vox-memo"
    frontmatter = (
        f"---\n"
        f"created: {timestamp.isoformat()}\n"
        f"source: vox-memo\n"
        f"tags:\n{tag_lines}\n"
        f"---\n"
    )

    # Sanitize title for filename (cap at 100 chars to avoid OS filename limits)
    safe_title = "".join(c if c.isalnum() or c in " -_" else "" for c in title).strip()[:100]
    if not safe_title:
        safe_title = timestamp.strftime("%Y%m%d_%H%M%S")
    filename = f"{safe_title}.md"

    # Avoid overwriting existing files
    out_path = config.OBSIDIAN_INBOX / filename
    counter = 1
    while out_path.exists():
        out_path = config.OBSIDIAN_INBOX / f"{safe_title} ({counter}).md"
        counter += 1

    out_path.write_text(f"{frontmatter}\n# {title}\n\n{content}\n", encoding="utf-8")
    print(f"Wrote: {out_path}")
    return out_path


@app.post("/memo")
async def receive_memo(request: Request):
    """Receive WAV audio from ESP32, transcribe, cleanup, save to Obsidian."""
    # Parse timestamp from header or use now
    ts_header = request.headers.get("X-Memo-Timestamp", "")
    try:
        # Parse as UTC-aware so frontmatter timestamps are always consistent
        timestamp = datetime.strptime(ts_header, "%Y%m%d_%H%M%S").replace(tzinfo=timezone.utc)
    except (ValueError, TypeError):
        timestamp = datetime.now(timezone.utc)

    # Check for duplicate (same timestamp already processed)
    ts_slug = timestamp.strftime("%Y%m%d_%H%M%S")
    existing = list(config.OBSIDIAN_INBOX.glob(f"*{ts_slug}*.md"))
    if existing:
        return {"status": "duplicate", "title": existing[0].stem, "preview": "", "filename": existing[0].name}

    # Reject oversized uploads before reading body into RAM (~5MB limit)
    content_length = request.headers.get("content-length")
    if content_length is not None and int(content_length) > 5 * 1024 * 1024:
        return Response(status_code=413, content="Request too large")

    # Save uploaded audio to temp file
    body = await request.body()
    if len(body) < 44:  # WAV header is 44 bytes minimum
        return Response(status_code=400, content="Audio too short")

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp.write(body)
        tmp_path = Path(tmp.name)

    try:
        # Optionally archive audio
        if config.ARCHIVE_AUDIO:
            archive_dir = config.OBSIDIAN_INBOX / "audio"
            archive_dir.mkdir(exist_ok=True)
            (archive_dir / f"{ts_slug}.wav").write_bytes(body)

        # Step 1: Transcribe
        try:
            raw_text = transcribe(tmp_path)
        except OpenAIError as e:
            print(f"Transcription error: {e}")
            return Response(status_code=503, content="Transcription service unavailable")
        if raw_text is None:
            return {"status": "silent", "title": "", "preview": "", "filename": ""}
        if not raw_text.strip():
            return {"status": "empty", "title": "", "preview": "", "filename": ""}

        # Step 2: Write raw transcription immediately (safety net)
        raw_title = f"Voice Memo {ts_slug}"
        raw_path = write_memo(timestamp, raw_title, raw_text, ["vox-memo", "raw"])

        # Step 3: LLM cleanup (overwrites raw if successful)
        cleaned = cleanup(raw_text)
        if cleaned and cleaned.get("content"):
            title = cleaned.get("title", raw_title)
            content = cleaned["content"]
            tags = cleaned.get("tags", [])
            if "vox-memo" not in tags:
                tags.insert(0, "vox-memo")
            # Write clean version first, then remove raw (order matters: if write fails, raw survives)
            final_path = write_memo(timestamp, title, content, tags)
            raw_path.unlink()
        else:
            title = raw_title
            content = raw_text
            final_path = raw_path

        return {
            "status": "ok",
            "title": title,
            "preview": content[:50],
            "filename": final_path.name,
        }
    finally:
        tmp_path.unlink(missing_ok=True)


@app.get("/health")
async def health():
    """Health check endpoint for ESP32 to verify server reachability."""
    return {"status": "ok", "max_duration": config.MAX_MEMO_DURATION_SECONDS}


if __name__ == "__main__":
    debug = os.getenv("DEBUG", "").lower() in ("1", "true", "yes")
    uvicorn.run("server:app", host=config.HOST, port=config.PORT, reload=debug, log_level="debug")
