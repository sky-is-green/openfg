#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the figures used by docs/WHITEPAPER.md.

The diagrams and charts share one palette and are written to
docs/assets/whitepaper/. Re-run after touching the measured data.

  python3 tools/make_paper_figures.py
"""

import os
import re
import sys
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "assets" / "whitepaper"

BG = "#fbfaf7"
PANEL = "#f3f0e9"
INK = "#1d232a"
MUTED = "#5f6b76"
GRID = "#d8d3c8"
BLUE = "#2f6f9f"
BLUE_L = "#7fa8c4"
ORANGE = "#c26a3d"
ORANGE_L = "#dfa07e"
GREEN = "#4f8a5b"
GREEN_L = "#8fba97"
GRAY = "#8b9299"

plt.rcParams.update({
    "figure.facecolor": BG,
    "savefig.facecolor": BG,
    "axes.facecolor": BG,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK,
    "axes.titlecolor": INK,
    "text.color": INK,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "grid.color": GRID,
    "font.family": "DejaVu Sans",
    "font.size": 9.5,
})


def box(ax, x, y, w, h, title, detail="", color=BLUE, fill=PANEL):
    patch = FancyBboxPatch(
        (x, y), w, h,
        boxstyle="round,pad=0.02,rounding_size=0.10",
        linewidth=1.3, edgecolor=color, facecolor=fill, zorder=2)
    ax.add_patch(patch)
    if detail:
        ax.text(x + w / 2, y + h * 0.67, title, ha="center", va="center",
                fontsize=10.5, fontweight="bold", color=INK, zorder=3)
        ax.text(x + w / 2, y + h * 0.30, detail, ha="center", va="center",
                fontsize=7.6, color=MUTED, linespacing=1.25, zorder=3)
    else:
        ax.text(x + w / 2, y + h / 2, title, ha="center", va="center",
                fontsize=9.5, fontweight="bold", color=INK, zorder=3)


def arrow(ax, start, end, label="", color=BLUE, both=False, rad=0.0):
    style = "<|-|>" if both else "-|>"
    patch = FancyArrowPatch(
        start, end, arrowstyle=style, mutation_scale=12,
        linewidth=1.25, color=color, shrinkA=2, shrinkB=2,
        connectionstyle=f"arc3,rad={rad}", zorder=1)
    ax.add_patch(patch)
    if label:
        mx = (start[0] + end[0]) / 2
        my = (start[1] + end[1]) / 2
        ax.text(mx, my + 0.10, label, ha="center", va="bottom",
                fontsize=7.4, color=color,
                bbox=dict(boxstyle="round,pad=0.18", fc=BG, ec="none"),
                zorder=4)


def save(fig, name):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=BG)
    plt.close(fig)
    print(f"wrote {path.relative_to(ROOT)}")


def architecture():
    fig, ax = plt.subplots(figsize=(14.0, 6.4))
    ax.set_xlim(0, 14.0)
    ax.set_ylim(0, 6.4)
    ax.set_aspect("equal")
    ax.axis("off")

    box(ax, 0.2, 2.3, 2.6, 1.7, "Vulkan app",
        "swapchain images\nrendered at app rate", color=GRAY)
    box(ax, 3.4, 2.1, 3.2, 2.0, "OpenFG layer",
        "hooks · pacing\ncopies · present order", color=BLUE)
    box(ax, 7.4, 3.6, 2.9, 1.5, "Compute backend",
        "flow or FidelityFX\ninterpolation · UI mask", color=ORANGE)
    box(ax, 7.4, 1.2, 2.9, 1.5, "Shared images",
        "source 2 layers\ndestination 3 layers", color=GREEN)
    box(ax, 11.1, 2.4, 2.5, 1.6, "Display",
        "compositor\nreceives presents", color=GRAY)
    box(ax, 3.4, 0.3, 3.2, 0.9, "Config · CLI · GUI",
        "TOML + OPENFG_*", color=GRAY)

    arrow(ax, (2.8, 3.15), (3.4, 3.15), "frames")
    arrow(ax, (6.6, 2.65), (7.4, 2.40), "")
    ax.text(7.02, 2.76, "copy A/B", ha="center", va="bottom",
            fontsize=7.2, color=BLUE)
    arrow(ax, (7.4, 1.90), (6.6, 2.15), "", color=ORANGE)
    ax.text(7.02, 1.64, "generated", ha="center", va="top",
            fontsize=7.2, color=ORANGE)
    arrow(ax, (8.85, 2.70), (8.85, 3.60), "", both=True, color=ORANGE)
    ax.text(9.05, 3.15, "read / write", ha="left", va="center",
            fontsize=7.2, color=ORANGE)

    # Route the present path above the shared/backend column.
    ax.plot([5.0, 5.0, 12.35], [4.10, 5.55, 5.55],
            color=BLUE, lw=1.25, zorder=1)
    arrow(ax, (12.35, 5.55), (12.35, 4.00), "", color=BLUE)
    ax.text(8.55, 5.64, "generated + real presents", ha="center",
            va="bottom", fontsize=7.6, color=BLUE,
            bbox=dict(boxstyle="round,pad=0.18", fc=BG, ec="none"))

    arrow(ax, (5.0, 1.20), (5.0, 2.10), "", color=GRAY)
    ax.text(5.15, 1.60, "config", ha="left", va="center",
            fontsize=7.2, color=MUTED)

    ax.text(13.6, 6.15, "Reference data path", ha="right", va="center",
            fontsize=9, color=MUTED, style="italic")
    save(fig, "architecture.png")


def timeline():
    fig, ax = plt.subplots(figsize=(12.2, 4.2))
    ax.set_xlim(0, 12.2)
    ax.set_ylim(0, 4.2)
    ax.axis("off")

    rows = [
        ("2x", 3.0, [("t = 0.5", 1)]),
        ("3x", 2.0, [("t = 1/3", 1), ("t = 2/3", 1)]),
        ("4x", 1.0, [("t = 0.25", 1), ("t = 0.5", 1), ("t = 0.75", 1)]),
    ]
    x0 = 1.55
    step = 1.55
    w = 1.25
    h = 0.62
    for label, y, gens in rows:
        ax.text(0.35, y + h / 2, label, ha="left", va="center",
                fontsize=11, fontweight="bold", color=INK)
        box(ax, x0, y, w, h, "A", "", color=BLUE, fill="#e7eef4")
        x = x0 + step
        for text, _ in gens:
            box(ax, x, y, w, h, text, "", color=ORANGE, fill="#f6e9e1")
            x += step
        box(ax, x, y, w, h, "B", "", color=BLUE, fill="#e7eef4")
        ax.annotate("", xy=(x + w, y + h / 2), xytext=(x0, y + h / 2),
                    arrowprops=dict(arrowstyle="-|>", color=GRID, lw=1.0,
                                    shrinkA=0, shrinkB=0))
        ax.text(x + w + 0.15, y + h / 2,
                "presented last", ha="left", va="center",
                fontsize=7.8, color=MUTED)

    ax.text(1.55, 3.95, "Presentation order  (left to right)", fontsize=9,
            color=MUTED)
    ax.text(0.35, 0.25,
            "Blue = real source frame. Orange = generated frame. "
            "The real frame is always presented after the generated ones, "
            "which adds display delay.",
            fontsize=8, color=MUTED)
    save(fig, "timeline.png")


def pipelines():
    fig, ax = plt.subplots(figsize=(14.4, 5.6))
    ax.set_xlim(0, 14.4)
    ax.set_ylim(0, 5.6)
    ax.axis("off")

    flow = [
        ("luma A/B", ""), ("downsample\npyramid", ""), ("tile mask", ""),
        ("block match\nL4", ""), ("median +\nsmooth", ""),
        ("LK refine\nL3–L1", ""), ("full-res\nrefine", ""),
    ]
    ffx = [
        ("prepare\nluma", ""), ("SPD\npyramid", ""), ("SCD", ""),
        ("search\nL n–0", ""), ("filter\nmedian", ""), ("scale\n2×2", ""),
        ("dense\nexpand", ""),
    ]
    x0 = 1.55
    step = 1.58
    w = 1.28
    h = 0.92
    for i, (title, _) in enumerate(flow):
        x = x0 + i * step
        box(ax, x, 3.75, w, h, title, "", color=BLUE, fill="#e7eef4")
        if i:
            arrow(ax, (x - step + w, 3.75 + h / 2), (x, 3.75 + h / 2),
                  color=BLUE)
    for i, (title, _) in enumerate(ffx):
        x = x0 + i * step
        box(ax, x, 1.65, w, h, title, "", color=ORANGE, fill="#f6e9e1")
        if i:
            arrow(ax, (x - step + w, 1.65 + h / 2), (x, 1.65 + h / 2),
                  color=ORANGE)
    interp_x = x0 + 7 * step + 0.05
    box(ax, interp_x, 1.55, 1.55, 3.85, "Interpolation",
        "warp · visibility\nUI mask · cut guard", color=GREEN,
        fill="#e9f0e8")
    arrow(ax, (x0 + 6 * step + w, 3.75 + h / 2),
          (interp_x, 3.75 + h / 2), color=BLUE)
    arrow(ax, (x0 + 6 * step + w, 1.65 + h / 2),
          (interp_x, 1.65 + h / 2), color=ORANGE)

    ax.text(0.15, 4.95, "OpenFG flow", fontsize=11, fontweight="bold",
            color=BLUE)
    ax.text(0.15, 2.85, "FidelityFX", fontsize=11, fontweight="bold",
            color=ORANGE)
    ax.text(0.15, 1.0, "FFX: 7 levels quality · 5 levels performance",
            fontsize=7.8, color=MUTED)
    ax.text(0.15, 0.55, "flow performance: no full-res refine, half-res field"
            " · FFX performance: half-size pyramid",
            fontsize=7.8, color=MUTED)
    save(fig, "pipelines.png")


def quality_cost():
    fig, (ax1, ax2) = plt.subplots(
        1, 2, figsize=(11.2, 4.5),
        gridspec_kw={"width_ratios": [1.35, 1.0], "wspace": 0.3})

    points = [
        ("flow", 1.56, 32.83, GRAY, "flow"),
        ("flow perf", 0.85, 32.50, GRAY, "flow perf"),
        ("ffx", 1.03, 33.23, BLUE, "FFX"),
        ("ffx perf", 0.52, 33.05, ORANGE, "FFX perf"),
    ]
    label_pos = {
        "flow": (1.58, 32.78),
        "flow perf": (0.88, 32.58),
        "ffx": (1.10, 33.28),
        "ffx perf": (0.60, 33.12),
    }
    for key, ms, db, color, label in points:
        ax1.scatter(ms, db, s=78, color=color, zorder=3,
                    edgecolor=BG, linewidth=1.2)
        ax1.annotate(label, label_pos[key], fontsize=8.5, color=color)
    ax1.set_xlabel("pan scene GPU time (ms)")
    ax1.set_ylabel("pan scene PSNR (dB)")
    ax1.set_title("Quality vs cost", loc="left", fontsize=11,
                  fontweight="bold")
    ax1.grid(True, alpha=0.7)
    ax1.annotate("better", xy=(0.65, 33.45), xytext=(1.20, 33.30),
                 arrowprops=dict(arrowstyle="-|>", color=MUTED, lw=1.1),
                 fontsize=8, color=MUTED)
    ax1.set_xlim(0.40, 1.78)
    ax1.set_ylim(32.40, 33.38)

    labels = ["flow", "flow perf", "FFX", "FFX perf"]
    values = [1.925, 1.951, 1.917, 1.947]
    colors = [GRAY, "#b9bec3", BLUE, ORANGE]
    bars = ax2.bar(labels, values, color=colors, width=0.62)
    for bar, value in zip(bars, values):
        ax2.text(bar.get_x() + bar.get_width() / 2, value + 0.004,
                 f"{value:.3f}", ha="center", va="bottom", fontsize=8)
    ax2.set_ylabel("ghost excess (lower is better)")
    ax2.set_title("Ghost gate", loc="left", fontsize=11,
                  fontweight="bold")
    ax2.set_ylim(1.88, 1.98)
    ax2.grid(True, axis="y", alpha=0.7)
    ax2.tick_params(axis="x", labelsize=8.5)
    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    save(fig, "quality-cost.png")


def hud_mask():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(9.2, 3.8))
    labels = ["mask off", "mask on"]
    colors = [GRAY, GREEN]
    values = [28.912, 1.280]
    bars = ax1.bar(labels, values, color=colors, width=0.55)
    for bar, value in zip(bars, values):
        ax1.text(bar.get_x() + bar.get_width() / 2, value + 0.5,
                 f"{value:.2f}", ha="center", va="bottom", fontsize=9)
    ax1.set_ylabel("halo ghost excess (lower is better)")
    ax1.set_title("Doubled-text energy", loc="left", fontsize=11,
                  fontweight="bold")
    ax1.set_ylim(0, 33)
    ax1.grid(True, axis="y", alpha=0.7)

    values = [37.09, 39.70]
    bars = ax2.bar(labels, values, color=colors, width=0.55)
    for bar, value in zip(bars, values):
        ax2.text(bar.get_x() + bar.get_width() / 2, value + 0.15,
                 f"{value:.2f} dB", ha="center", va="bottom", fontsize=9)
    ax2.set_ylabel("full-frame PSNR (higher is better)")
    ax2.set_title("Overall reconstruction", loc="left", fontsize=11,
                  fontweight="bold")
    ax2.set_ylim(35.5, 41.0)
    ax2.grid(True, axis="y", alpha=0.7)
    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    fig.text(0.01, 0.01, "48-pixel pan with static HUD text; the mask is "
             "sampled at uv, uva and uvb.", fontsize=7.6, color=MUTED,
             ha="left")
    save(fig, "hud-mask.png")


PERF = re.compile(
    r"^\[(\d\d:\d\d:\d\d\.\d+)\].*?perf: app ([\d.]+) fps, "
    r"presented ([\d.]+) fps, generated (\d+)%, gen time (\d+)%, "
    r"overhead avg ([\d.]+) ms,.*?generating (yes|no)")


def reference_session():
    log = Path(os.environ.get(
        "OPENFG_LOG", Path.home() / ".local/state/openfg/layer.log"))
    if not log.is_file():
        print(f"skip reference session: no {log}")
        return
    rows = []
    offset = 0.0
    previous = None
    for line in log.read_text(errors="replace").splitlines():
        match = PERF.search(line)
        if not match:
            continue
        stamp = datetime.strptime(match.group(1), "%H:%M:%S.%f")
        seconds = (stamp.hour * 3600 + stamp.minute * 60 + stamp.second +
                   stamp.microsecond / 1e6)
        if previous is not None and seconds + offset < previous - 43200:
            offset += 86400.0
        seconds += offset
        previous = seconds
        rows.append((seconds, float(match.group(2)), float(match.group(3)),
                     float(match.group(4)), float(match.group(6)),
                     match.group(7)))
    if not rows:
        print("skip reference session: no perf lines")
        return
    active = [r for r in rows if r[5] == "yes" and r[3] > 0 and r[4] > 0.2]
    if len(active) >= 6:
        rows = active
    segments = [[rows[0]]]
    for row in rows[1:]:
        if row[0] - segments[-1][-1][0] > 30.0:
            segments.append([row])
        else:
            segments[-1].append(row)
    segments = [s for s in segments if len(s) >= 6]
    if not segments:
        print("skip reference session: no continuous segment")
        return
    rows = segments[-1][-30:]
    # Drop a trailing stop/restart transition if the last report is still
    # labelled generating but presented fps has already collapsed.
    while len(rows) > 6 and rows[-1][2] > rows[-1][1] * 1.4:
        rows = rows[:-1]
    t0 = rows[0][0]
    times = [r[0] - t0 for r in rows]
    app = [r[1] for r in rows]
    presented = [r[2] for r in rows]
    overhead = [r[4] for r in rows]

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(10.5, 4.8), sharex=True,
        gridspec_kw={"height_ratios": [2.2, 1.0], "hspace": 0.10})
    ax1.plot(times, app, color=GRAY, lw=1.8, label="app fps")
    ax1.plot(times, presented, color=GREEN, lw=2.2,
             label="presented fps")
    ax1.fill_between(times, app, presented, color=GREEN, alpha=0.14)
    ax1.set_ylabel("frames / s")
    ax1.set_title("Monster Hunter Wilds, 1440p, 2x FFX — active generation",
                  loc="left", fontsize=11, fontweight="bold")
    ax1.grid(True, alpha=0.7)
    ax1.legend(loc="upper right", frameon=False)
    ax2.plot(times, overhead, color=BLUE, lw=1.6)
    ax2.set_ylabel("overhead ms")
    ax2.set_xlabel("seconds")
    ax2.grid(True, alpha=0.7)
    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    fig.text(0.01, 0.01, "Measured by the layer; overhead includes copy, "
             "generate and present submission.", fontsize=7.6,
             color=MUTED, ha="left")
    save(fig, "wilds-session.png")


def main() -> int:
    architecture()
    timeline()
    pipelines()
    quality_cost()
    hud_mask()
    reference_session()
    return 0


if __name__ == "__main__":
    sys.exit(main())
