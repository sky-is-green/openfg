#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record an X11/XWayland window or the full display at a fixed frame rate.

This is the capture helper for the OpenFG showcase. It uses ffmpeg's x11grab
so a full-screen Proton game can be recorded at 60 fps without the per-frame
overhead of `import`. Native and generated runs should use identical
duration, resolution and frame rate.

  # full screen, 60 s at 1440p60
  python3 tools/capture_window.py --fullscreen --duration 60 \
      --output showcase/raw/wilds-native.mp4

  # a window found by title substring
  python3 tools/capture_window.py --title "Monster Hunter Wilds" \
      --duration 60 --output showcase/raw/wilds-ffx2x.mp4

The agent must not send input to the game; launch and play the route yourself.
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time


def require(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise SystemExit(f"{name} not found in PATH")
    return path


def window_geometry(title: str) -> tuple[int, int, int, int, str]:
    xdotool = require("xdotool")
    try:
        ids = subprocess.check_output(
            [xdotool, "search", "--name", title], text=True,
            stderr=subprocess.DEVNULL).split()
    except subprocess.CalledProcessError:
        raise SystemExit(f"no X11/XWayland window matching {title!r}")
    if not ids:
        raise SystemExit(f"no X11/XWayland window matching {title!r}")

    for wid in ids:
        try:
            name = subprocess.check_output(
                [xdotool, "getwindowname", wid], text=True,
                stderr=subprocess.DEVNULL).strip()
            geom = subprocess.check_output(
                [xdotool, "getwindowgeometry", "--shell", wid], text=True,
                stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            continue
        values = {}
        for line in geom.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
        try:
            x = int(values["X"])
            y = int(values["Y"])
            width = int(values["WIDTH"])
            height = int(values["HEIGHT"])
        except (KeyError, ValueError):
            continue
        if width >= 100 and height >= 100:
            return x, y, width, height, name
    raise SystemExit(f"no usable window matching {title!r}")


def display_geometry() -> tuple[int, int]:
    xdotool = require("xdotool")
    out = subprocess.check_output(
        [xdotool, "getdisplaygeometry"], text=True).split()
    return int(out[0]), int(out[1])


def main() -> int:
    ap = argparse.ArgumentParser()
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--title", help="window title substring")
    target.add_argument("--fullscreen", action="store_true",
                        help="capture the whole display")
    ap.add_argument("--output", required=True)
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds; 0 records until Ctrl+C")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--crf", type=int, default=14)
    ap.add_argument("--preset", default="veryfast")
    ap.add_argument("--offset-x", type=int)
    ap.add_argument("--offset-y", type=int)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    ffmpeg = require("ffmpeg")
    if args.fullscreen:
        width, height = display_geometry()
        x = y = 0
        label = "display"
    else:
        x, y, width, height, label = window_geometry(args.title)

    if args.offset_x is not None:
        x = args.offset_x
    if args.offset_y is not None:
        y = args.offset_y

    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".",
                exist_ok=True)
    display = os.environ.get("DISPLAY", ":0")
    # x11grab wants display.screen; DISPLAY is often just ":0".
    if re.search(r":\d+$", display):
        display += ".0"
    cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "warning",
        "-thread_queue_size", "512",
        "-f", "x11grab",
        "-framerate", str(args.fps),
        "-video_size", f"{width}x{height}",
        "-draw_mouse", "0",
        "-i", f"{display}+{x},{y}",
    ]
    if args.duration > 0:
        cmd += ["-t", f"{args.duration:.3f}"]
    cmd += [
        "-an",
        "-c:v", "libx264",
        "-preset", args.preset,
        "-crf", str(args.crf),
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        args.output,
    ]

    print(f"source: {label}  region: {width}x{height}+{x}+{y}  "
          f"{args.fps} fps", flush=True)
    print("command:", shlex.join(cmd), flush=True)
    if args.dry_run:
        return 0

    started = time.monotonic()
    proc = subprocess.Popen(cmd)
    try:
        code = proc.wait()
    except KeyboardInterrupt:
        proc.send_signal(2)
        code = proc.wait()
    elapsed = time.monotonic() - started
    print(f"ffmpeg exit {code}, {elapsed:.1f}s, {args.output}", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
