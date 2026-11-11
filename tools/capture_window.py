#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record an X11/XWayland window or one monitor at a fixed frame rate.

A small ffmpeg x11grab wrapper for bug reports and before/after captures. It
avoids the per-frame overhead of `import` and keeps the output at a constant
frame rate.

  # one monitor, 30 s at 1440p60
  python3 tools/capture_window.py --monitor DP-1 --duration 30 \
      --output ~/Videos/openfg-captures/game.mp4

  # a window found by title substring
  python3 tools/capture_window.py --title "Game Window" \
      --duration 30 --output ~/Videos/openfg-captures/game.mp4

The agent must not send input to the game; launch and play it yourself.
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


def monitor_geometry(name: str) -> tuple[int, int, int, int]:
    """Return X, Y, width, height for one xrandr output.

    On a multi-monitor Xwayland root, getdisplaygeometry() reports the union
    of every monitor. A 2560x1440 primary next to a 1920x1080 secondary would
    otherwise be captured as 4480x1440, with the second monitor in frame.
    """
    xrandr = require("xrandr")
    try:
        out = subprocess.check_output(
            [xrandr, "--query"], text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"xrandr failed: {exc}")
    for line in out.splitlines():
        if not re.match(rf"^{re.escape(name)}\s+connected\b", line):
            continue
        match = re.search(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
        if match:
            width, height, x, y = (int(v) for v in match.groups())
            return x, y, width, height
    raise SystemExit(f"no connected xrandr output named {name!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--title", help="window title substring")
    target.add_argument("--fullscreen", action="store_true",
                        help="capture the whole X display (all monitors)")
    target.add_argument("--monitor",
                        help="xrandr output name, e.g. DP-1 (recommended)")
    ap.add_argument("--output", required=True)
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds; 0 records until Ctrl+C")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--crf", type=int, default=14)
    ap.add_argument("--preset", default="veryfast")
    ap.add_argument("--countdown", type=float, default=0.0,
                    help="seconds to wait before capturing; switch to the game")
    ap.add_argument("--beep", action="store_true",
                    help="play a system sound when the capture starts")
    ap.add_argument("--offset-x", type=int)
    ap.add_argument("--offset-y", type=int)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    ffmpeg = require("ffmpeg")
    if args.fullscreen:
        width, height = display_geometry()
        x = y = 0
        label = "display"
    elif args.monitor:
        x, y, width, height = monitor_geometry(args.monitor)
        label = args.monitor
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

    if args.countdown > 0:
        remaining = max(0, int(round(args.countdown)))
        for second in range(remaining, 0, -1):
            print(f"recording in {second}...", flush=True)
            time.sleep(1)
    print("recording now", flush=True)
    if args.beep:
        for player, sound in (
            ("paplay", "/usr/share/sounds/freedesktop/stereo/complete.oga"),
            ("canberra-gtk-play", None),
        ):
            if not shutil.which(player):
                continue
            cmd = [player] if sound is None else [player, sound]
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
                break
            except OSError:
                continue

    started = time.monotonic()
    proc = subprocess.Popen(cmd)
    try:
        code = proc.wait()
    except KeyboardInterrupt:
        proc.send_signal(2)
        code = proc.wait()
    elapsed = time.monotonic() - started
    print(f"ffmpeg exit {code}, {elapsed:.1f}s, {args.output}", flush=True)
    if code == 0:
        probe = shutil.which("ffprobe")
        if probe:
            summary = subprocess.run(
                [probe, "-v", "error", "-select_streams", "v:0",
                 "-show_entries", "stream=width,height,r_frame_rate",
                 "-show_entries", "format=duration", "-of", "default=nw=1",
                 args.output],
                text=True, capture_output=True, check=False)
            if summary.stdout.strip():
                print(summary.stdout.strip().replace("\n", "  "), flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
