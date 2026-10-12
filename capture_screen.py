#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5,<4", "Pillow>=10,<13"]
# ///
"""Capture the ESPJaalee framebuffer over USB serial and save a PNG.

Upload the screenshot-enabled sketch and close Arduino Serial Monitor first.
Run: uv run capture_screen.py --port /dev/cu.usbserial-XXXX --output screenshot.png
List ports: uv run capture_screen.py --list-ports
"""

import argparse
from datetime import datetime
from pathlib import Path
import sys
import time


def read_exact(connection, count, deadline):
    """Serial reads can return partial data; enforce an overall transfer timeout."""
    data = bytearray()
    while len(data) < count:
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Screenshot timed out ({len(data)}/{count} bytes received)")
        data.extend(connection.read(count - len(data)))
    return data


def capture(connection):
    # Avoid intentional ESP32 resets via DTR/RTS. Some USB drivers still toggle
    # these on open, so allow startup to finish before requesting a screenshot.
    time.sleep(2)
    connection.reset_input_buffer()
    connection.write(b"s")
    connection.flush()
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        line = connection.readline()
        if line.startswith(b"ESPJAALEE_SCREENSHOT_ERROR"):
            raise RuntimeError(line.decode("ascii", errors="replace").strip())
        if line.startswith(b"ESPJAALEE_SCREENSHOT "):
            fields = line.split()
            if len(fields) != 4 or fields[3] != b"RGB565":
                raise RuntimeError("Unrecognized screenshot header")
            width, height = int(fields[1]), int(fields[2])
            if not (0 < width <= 1024 and 0 < height <= 1024):
                raise RuntimeError("Invalid screenshot dimensions")
            pixels = read_exact(connection, width * height * 2, deadline)
            trailer = b"\nESPJAALEE_SCREENSHOT_END\n"
            if read_exact(connection, len(trailer), deadline) != trailer:
                raise RuntimeError("Screenshot transfer was incomplete or corrupted")
            return width, height, pixels
    raise RuntimeError("No screenshot response; upload the updated sketch and close Serial Monitor")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial device; auto-selected if exactly one is found")
    parser.add_argument("--list-ports", action="store_true", help="List available serial devices")
    parser.add_argument("--output", type=Path,
                        default=Path(datetime.now().strftime("screenshot-%Y%m%d-%H%M%S.png")))
    args = parser.parse_args()
    try:
        import serial
        from serial.tools import list_ports
        from PIL import Image
    except ImportError:
        parser.exit(1, "Run with automatic dependencies: uv run capture_screen.py\n")

    ports = list(list_ports.comports())
    if args.list_ports:
        for port in ports:
            print(f"{port.device}  {port.description}")
        if not ports:
            print("No serial devices found")
        return 0
    if not args.port:
        if len(ports) != 1:
            parser.error("Specify --port; use --list-ports to find your T-Display")
        args.port = ports[0].device

    try:
        connection = serial.Serial(baudrate=115200, timeout=0.5, write_timeout=5)
        connection.dtr = False
        connection.rts = False
        connection.port = args.port
        with connection:
            print(f"Capturing from {args.port}…", flush=True)
            width, height, pixels = capture(connection)

        # The firmware sends row-major RGB565 with the most significant byte first.
        rgb = bytearray(width * height * 3)
        for index in range(width * height):
            pixel = (pixels[index * 2] << 8) | pixels[index * 2 + 1]
            red = (pixel >> 11) & 0x1F
            green = (pixel >> 5) & 0x3F
            blue = pixel & 0x1F
            rgb[index * 3:index * 3 + 3] = bytes((
                (red << 3) | (red >> 2),
                (green << 2) | (green >> 4),
                (blue << 3) | (blue >> 2),
            ))
        Image.frombytes("RGB", (width, height), bytes(rgb)).save(args.output, format="PNG")
        print(f"Saved {width} × {height} screenshot to {args.output.resolve()}")
    except (serial.SerialException, OSError, RuntimeError, ValueError) as error:
        print(f"Capture failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nCapture cancelled", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
