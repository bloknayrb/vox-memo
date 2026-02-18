#!/usr/bin/env python3
"""
Vox Memo USB Sync — transfer memos from ESP32 via USB Serial/JTAG.

Connects to the device over serial, lists memos, downloads each one,
and POSTs it to the running Vox Memo server for transcription.

Usage:
    uv run usb_sync.py                          # one-shot sync
    uv run usb_sync.py --watch                   # poll every 30s while plugged in
    uv run usb_sync.py --port COM3 --watch 10    # custom port, 10s interval

Requires: pyserial, httpx
"""

import argparse
import sys
import time
import zlib

import httpx
import serial
import serial.tools.list_ports


def find_esp32_port() -> str | None:
    """Auto-detect the ESP32-C6 USB Serial/JTAG port."""
    for port in serial.tools.list_ports.comports():
        desc = (port.description or "").lower()
        hwid = (port.hwid or "").lower()
        # ESP32-C6 USB Serial/JTAG shows up as:
        #   "USB JTAG/serial debug unit" (some drivers)
        #   "USB Serial Device" (generic Windows CDC driver)
        # Also match by USB VID:PID — Espressif = 303A:1001
        if "jtag" in desc or "esp" in desc or "303a" in hwid:
            return port.device
    return None


def send_cmd(ser: serial.Serial, cmd: str) -> None:
    """Send a command string followed by newline."""
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()


def read_line(ser: serial.Serial, timeout: float = 5.0) -> str:
    """Read a line from serial with timeout."""
    ser.timeout = timeout
    line = ser.readline()
    if not line:
        raise TimeoutError("No response from device")
    return line.decode("ascii", errors="replace").strip()


def list_memos(ser: serial.Serial) -> list[tuple[str, int]]:
    """Send LIST command and parse file entries."""
    send_cmd(ser, "LIST")
    memos = []
    while True:
        line = read_line(ser)
        if line == "END":
            break
        if line.startswith("FILE "):
            parts = line.split()
            if len(parts) >= 3:
                name = parts[1]
                size = int(parts[2])
                memos.append((name, size))
    return memos


def download_memo(ser: serial.Serial, name: str, expected_size: int) -> bytes:
    """Send GET command and receive file data with CRC verification."""
    send_cmd(ser, f"GET {name}")

    header = read_line(ser, timeout=10.0)
    if header.startswith("ERR"):
        raise RuntimeError(f"Device error: {header}")

    if not header.startswith("DATA "):
        raise RuntimeError(f"Unexpected response: {header}")

    size = int(header.split()[1])
    if size != expected_size:
        print(f"  Warning: expected {expected_size} bytes, device says {size}")

    # Read raw binary data
    data = b""
    remaining = size
    ser.timeout = 30.0
    while remaining > 0:
        chunk = ser.read(min(remaining, 4096))
        if not chunk:
            raise TimeoutError(f"Stalled after {len(data)}/{size} bytes")
        data += chunk
        remaining -= len(chunk)

    # Read CRC line
    crc_line = read_line(ser)
    if not crc_line.startswith("CRC32 "):
        raise RuntimeError(f"Expected CRC32, got: {crc_line}")

    device_crc = int(crc_line.split()[1], 16)
    local_crc = zlib.crc32(data) & 0xFFFFFFFF
    if device_crc != local_crc:
        raise RuntimeError(
            f"CRC mismatch: device={device_crc:08X} local={local_crc:08X}"
        )

    return data


def delete_memo(ser: serial.Serial, name: str) -> bool:
    """Send DELETE command."""
    send_cmd(ser, f"DELETE {name}")
    response = read_line(ser)
    return response == "OK"


def upload_to_server(server_url: str, name: str, wav_data: bytes) -> dict:
    """POST WAV data to the Vox Memo server."""
    # Extract timestamp from filename (e.g., "20260217_153000.wav" -> "20260217_153000")
    timestamp = name.rsplit(".", 1)[0] if "." in name else name

    response = httpx.post(
        f"{server_url}/memo",
        content=wav_data,
        headers={
            "Content-Type": "audio/wav",
            "X-Memo-Timestamp": timestamp,
        },
        timeout=60.0,
    )
    response.raise_for_status()
    return response.json()


def connect(port: str | None, baud: int) -> serial.Serial | None:
    """Connect to the device and verify with PING. Returns None on failure."""
    port = port or find_esp32_port()
    if not port:
        return None

    try:
        ser = serial.Serial(port, baud, timeout=5)
        time.sleep(0.5)
        send_cmd(ser, "PING")
        response = read_line(ser, timeout=3.0)
        if response != "PONG":
            ser.close()
            return None
        return ser
    except (serial.SerialException, TimeoutError, OSError):
        return None


def sync_once(ser: serial.Serial, server_url: str, keep_files: bool) -> int:
    """Run one sync cycle. Returns number of memos successfully transferred."""
    memos = list_memos(ser)
    if not memos:
        return 0

    # Check server is up before transferring
    try:
        health = httpx.get(f"{server_url}/health", timeout=5.0)
        health.raise_for_status()
    except Exception as e:
        print(f"  Server not reachable: {e}")
        return 0

    success_count = 0
    for name, size in memos:
        try:
            wav_data = download_memo(ser, name, size)
            result = upload_to_server(server_url, name, wav_data)
            title = result.get("title", "")
            status = result.get("status", "")
            print(f"  {name} -> \"{title}\" ({status})")

            if not keep_files and status in ("ok", "duplicate"):
                delete_memo(ser, name)

            success_count += 1
        except Exception as e:
            print(f"  {name}: error — {e}")

    return success_count


def main():
    parser = argparse.ArgumentParser(description="Vox Memo USB Sync")
    parser.add_argument(
        "--port", "-p",
        help="Serial port (auto-detected if not specified)",
    )
    parser.add_argument(
        "--server", "-s",
        default="http://localhost:8000",
        help="Vox Memo server URL (default: http://localhost:8000)",
    )
    parser.add_argument(
        "--no-delete",
        action="store_true",
        help="Don't delete memos from device after successful transfer",
    )
    parser.add_argument(
        "--baud", "-b",
        type=int,
        default=115200,
        help="Baud rate (default: 115200)",
    )
    parser.add_argument(
        "--watch", "-w",
        nargs="?",
        const=30,
        type=int,
        metavar="SECONDS",
        help="Watch mode: poll for new memos every N seconds (default: 30)",
    )
    args = parser.parse_args()

    if args.watch is not None:
        # --- Watch mode: reconnect loop ---
        interval = args.watch
        print(f"Watch mode — polling every {interval}s (Ctrl+C to stop)")
        ser = None

        try:
            while True:
                # (Re)connect if needed
                if ser is None or not ser.is_open:
                    port = args.port or find_esp32_port()
                    if port:
                        ser = connect(port, args.baud)
                        if ser:
                            print(f"Connected to {port}")
                        else:
                            ser = None
                    else:
                        ser = None

                if ser is None:
                    # Device not plugged in — wait and retry
                    time.sleep(interval)
                    continue

                try:
                    synced = sync_once(ser, args.server, args.no_delete)
                    if synced > 0:
                        print(f"  Synced {synced} memo(s)")
                except (serial.SerialException, TimeoutError, OSError) as e:
                    # Device disconnected
                    print(f"Device disconnected ({e})")
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None

                time.sleep(interval)

        except KeyboardInterrupt:
            print("\nStopped")
            if ser and ser.is_open:
                ser.close()
        return

    # --- One-shot mode ---
    port = args.port or find_esp32_port()
    if not port:
        print("Error: No ESP32 USB port found. Specify with --port")
        sys.exit(1)

    print(f"Connecting to {port}...")
    ser = connect(port, args.baud)
    if not ser:
        print("Error: Device not responding. Is USB sync firmware running?")
        sys.exit(1)

    print("Connected to Vox Memo device")

    memos = list_memos(ser)
    if not memos:
        print("No memos on device.")
        ser.close()
        return

    print(f"Found {len(memos)} memo(s):")
    for name, size in memos:
        print(f"  {name} ({size:,} bytes)")

    try:
        health = httpx.get(f"{args.server}/health", timeout=5.0)
        health.raise_for_status()
        print(f"Server OK at {args.server}")
    except Exception as e:
        print(f"Error: Server not reachable at {args.server}: {e}")
        ser.close()
        sys.exit(1)

    synced = sync_once(ser, args.server, args.no_delete)
    ser.close()
    print(f"\nDone: {synced}/{len(memos)} memos synced")


if __name__ == "__main__":
    main()
