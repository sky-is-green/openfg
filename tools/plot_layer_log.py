#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Plot the layer's perf lines for a showcase overlay.

Reads ~/.local/state/openfg/layer.log (or OPENFG_LOG / --input), extracts the
five-second perf reports and writes a dark PNG/SVG with app/presented fps and
average layer overhead.

  python3 tools/plot_layer_log.py --tail 240 --output showcase/fps.png
"""

import argparse
import os
import re
import sys
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERF = re.compile(
    r"^\[(\d\d:\d\d:\d\d\.\d+)\].*?perf: app ([\d.]+) fps, "
    r"presented ([\d.]+) fps, generated (\d+)%, gen time (\d+)%, "
    r"overhead avg ([\d.]+) ms")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", type=Path,
                    default=Path(os.environ.get(
                        "OPENFG_LOG",
                        Path.home() / ".local/state/openfg/layer.log")))
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--tail", type=int, default=0,
                    help="use only the last N matching perf lines")
    ap.add_argument("--max-gap", type=float, default=30.0,
                    help="seconds; split sessions at larger gaps")
    ap.add_argument("--segment", choices=("last", "longest"), default="last",
                    help="which continuous session to plot")
    ap.add_argument("--title", default="OpenFG layer")
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"no log at {args.input}", file=sys.stderr)
        return 1

    rows = []
    day_offset = 0.0
    previous = None
    for line in args.input.read_text(errors="replace").splitlines():
        match = PERF.search(line)
        if not match:
            continue
        stamp = datetime.strptime(match.group(1), "%H:%M:%S.%f")
        seconds = stamp.hour * 3600 + stamp.minute * 60 + stamp.second + \
            stamp.microsecond / 1e6
        if previous is not None and seconds + day_offset < previous - 43200:
            day_offset += 86400.0
        seconds += day_offset
        previous = seconds
        rows.append((seconds,
                     float(match.group(2)), float(match.group(3)),
                     float(match.group(4)), float(match.group(6))))
    if not rows:
        print("no perf lines found", file=sys.stderr)
        return 1
    # The log contains many sessions separated by minutes or hours. Plot one
    # continuous run so a loading screen or a later session does not flatten
    # the overlay.
    segments = [[rows[0]]]
    for row in rows[1:]:
        if row[0] - segments[-1][-1][0] > args.max_gap:
            segments.append([row])
        else:
            segments[-1].append(row)
    segments = [s for s in segments if len(s) >= 2]
    if not segments:
        print("no continuous perf segment found", file=sys.stderr)
        return 1
    rows = max(segments, key=len) if args.segment == "longest" else segments[-1]
    if args.tail > 0:
        rows = rows[-args.tail:]
    t0 = rows[0][0]
    times = [r[0] - t0 for r in rows]
    app = [r[1] for r in rows]
    presented = [r[2] for r in rows]
    generated = [r[3] for r in rows]
    overhead = [r[4] for r in rows]

    plt.rcParams.update({
        "figure.facecolor": "#12161b",
        "axes.facecolor": "#12161b",
        "axes.edgecolor": "#3a4350",
        "axes.labelcolor": "#e6e8eb",
        "text.color": "#e6e8eb",
        "xtick.color": "#9aa0a6",
        "ytick.color": "#9aa0a6",
        "grid.color": "#2b333d",
        "font.family": "DejaVu Sans",
        "font.size": 10,
    })
    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(9.0, 4.2), sharex=True,
        gridspec_kw={"height_ratios": [2.2, 1.0], "hspace": 0.12})
    ax1.plot(times, app, color="#9aa0a6", linewidth=1.8, label="app fps")
    ax1.plot(times, presented, color="#7ec699", linewidth=2.2,
             label="presented fps")
    ax1.fill_between(times, app, presented, color="#7ec699", alpha=0.12)
    ax1.set_ylabel("frames / s")
    ax1.grid(True, alpha=0.5)
    ax1.legend(loc="upper right", frameon=False)
    ax1.set_title(args.title, loc="left", fontsize=12, fontweight="bold")
    ax2.plot(times, overhead, color="#9ecbff", linewidth=1.6)
    ax2.set_ylabel("overhead ms")
    ax2.set_xlabel("seconds")
    ax2.grid(True, alpha=0.5)
    if times:
        ax2.set_xlim(times[0], max(times[-1], times[0] + 1.0))
    ax1.set_xlim(ax2.get_xlim())
    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=180, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"wrote {args.output} ({len(rows)} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
