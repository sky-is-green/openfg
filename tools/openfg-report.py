#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarise OpenFG layer logs.

Parses the `session:`, `perf:`, `path:`, `adaptive:` and startup lines and
prints a compact Markdown table. Useful for comparing captures and for
checking gate churn without opening the raw log.

Examples:

    python3 tools/openfg-report.py raw/tina2k-ffx2x-30.log
    python3 tools/openfg-report.py raw/*.log --detail
    python3 tools/openfg-report.py raw/tina2k-churn-120.log --json
"""

import argparse
import json
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

SESSION_RE = re.compile(
    r"session:\s*([\d.]+) s, app ([\d.]+) fps, presented ([\d.]+) fps, "
    r"generated (\d+)%, gen time (\d+)%, overhead avg ([\d.]+) ms, "
    r"present p50 ([\d.]+) / p95 ([\d.]+) / p99 ([\d.]+) ms, "
    r"frame p50 ([\d.]+) / p95 ([\d.]+) ms")
PERF_RE = re.compile(
    r"perf: app ([\d.]+) fps, presented ([\d.]+) fps, generated (\d+)%, "
    r"gen time (\d+)%, overhead avg ([\d.]+) ms, frame p50 ([\d.]+) ms, "
    r"p95 ([\d.]+) ms, generating (yes|no)")
PATH_RE = re.compile(
    r"path: source ([\d.]+) ms, generate ([\d.]+) ms, "
    r"copies ([\d.]+) ms, present ([\d.]+) ms \((\d+) frames\)")
START_RE = re.compile(r"adaptive: start generating \(([^)]*)\)")
STOP_RE = re.compile(r"adaptive: stop generating \(([^)]*)\)")
INIT_RE = re.compile(r"backend init: ([\d.]+) ms")
FIRST_RE = re.compile(r"first generated frame \(([\d.]+) ms after start")
DISPLAY_RE = re.compile(r"display: (\S+) (\d+x\d+) @ ([\d.]+) Hz")


@dataclass
class Session:
    elapsed: float
    app_fps: float
    presented_fps: float
    generated_pct: float
    gen_time_pct: float
    overhead_ms: float
    present_p50: float
    present_p95: float
    present_p99: float
    frame_p50: float
    frame_p95: float


@dataclass
class Perf:
    app_fps: float
    presented_fps: float
    generated_pct: float
    gen_time_pct: float
    overhead_ms: float
    frame_p50: float
    frame_p95: float
    generating: bool


@dataclass
class PathSample:
    source_ms: float
    generate_ms: float
    copies_ms: float
    present_ms: float
    frames: int


@dataclass
class Report:
    name: str
    sessions: list[Session] = field(default_factory=list)
    perf: list[Perf] = field(default_factory=list)
    paths: list[PathSample] = field(default_factory=list)
    starts: int = 0
    stops: int = 0
    inits: list[float] = field(default_factory=list)
    first_frames: list[float] = field(default_factory=list)
    display: str = ""


def parse_text(text, name="log"):
    report = Report(name=name)
    for line in text.splitlines():
        match = SESSION_RE.search(line)
        if match:
            report.sessions.append(Session(*map(float, match.groups())))
            continue
        match = PERF_RE.search(line)
        if match:
            values = match.groups()
            report.perf.append(Perf(
                app_fps=float(values[0]),
                presented_fps=float(values[1]),
                generated_pct=float(values[2]),
                gen_time_pct=float(values[3]),
                overhead_ms=float(values[4]),
                frame_p50=float(values[5]),
                frame_p95=float(values[6]),
                generating=values[7] == "yes"))
            continue
        match = PATH_RE.search(line)
        if match:
            values = match.groups()
            report.paths.append(PathSample(
                source_ms=float(values[0]),
                generate_ms=float(values[1]),
                copies_ms=float(values[2]),
                present_ms=float(values[3]),
                frames=int(values[4])))
            continue
        if START_RE.search(line):
            report.starts += 1
        elif STOP_RE.search(line):
            report.stops += 1
        match = INIT_RE.search(line)
        if match:
            report.inits.append(float(match.group(1)))
        match = FIRST_RE.search(line)
        if match:
            report.first_frames.append(float(match.group(1)))
        match = DISPLAY_RE.search(line)
        if match:
            report.display = f"{match.group(1)} {match.group(2)} @ {match.group(3)} Hz"
    return report


def parse_file(path):
    return parse_text(path.read_text(encoding="utf-8", errors="replace"),
                      name=path.name)


def summary(report):
    session = report.sessions[-1] if report.sessions else None
    perf = report.perf
    app_fps = statistics.mean(p.app_fps for p in perf) if perf else (
        session.app_fps if session else 0.0)
    presented_fps = statistics.mean(p.presented_fps for p in perf) if perf else (
        session.presented_fps if session else 0.0)
    generated = statistics.mean(p.generated_pct for p in perf) if perf else (
        session.generated_pct if session else 0.0)
    overheads = [p.overhead_ms for p in perf]
    frames = [p.frame_p50 for p in perf]
    paths = report.paths
    return {
        "log": report.name,
        "display": report.display,
        "duration_s": session.elapsed if session else 0.0,
        "app_fps": app_fps,
        "presented_fps": presented_fps,
        "generated_pct": generated,
        "overhead_mean": statistics.mean(overheads) if overheads else (
            session.overhead_ms if session else 0.0),
        "overhead_median": statistics.median(overheads) if overheads else 0.0,
        "overhead_max": max(overheads) if overheads else 0.0,
        "frame_p50": statistics.median(frames) if frames else (
            session.frame_p50 if session else 0.0),
        "present_available": bool(session and (
            session.present_p50 > 0.0 or session.present_p95 > 0.0 or
            session.present_p99 > 0.0)),
        "present_p50": session.present_p50 if session else 0.0,
        "present_p95": session.present_p95 if session else 0.0,
        "present_p99": session.present_p99 if session else 0.0,
        "starts": report.starts,
        "stops": report.stops,
        "restarts": max(0, len(report.inits) - 1),
        "init_mean": statistics.mean(report.inits) if report.inits else 0.0,
        "restart_init_mean": (statistics.mean(report.inits[1:])
                             if len(report.inits) > 1 else 0.0),
        "first_frame_mean": (statistics.mean(report.first_frames)
                             if report.first_frames else 0.0),
        "source_ms": statistics.mean(p.source_ms for p in paths) if paths else 0.0,
        "generate_ms": statistics.mean(p.generate_ms for p in paths) if paths else 0.0,
        "present_ms": statistics.mean(p.present_ms for p in paths) if paths else 0.0,
        "frames": sum(p.frames for p in paths),
    }


def format_table(rows):
    headers = [
        "log", "duration", "app", "presented", "gen", "overhead",
        "p50", "present p50", "starts/stops",
    ]
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join(["---"] * len(headers)) + "|",
    ]
    for row in rows:
        present = (f"{row['present_p50']:.1f} ms"
                   if row['present_available'] else "n/a")
        lines.append(
            f"| {row['log']} | {row['duration_s']:.1f} s | "
            f"{row['app_fps']:.1f} | {row['presented_fps']:.1f} | "
            f"{row['generated_pct']:.0f}% | {row['overhead_mean']:.2f} ms | "
            f"{row['frame_p50']:.1f} ms | {present} | "
            f"{row['starts']}/{row['stops']} |")
    return "\n".join(lines)


def format_detail(row):
    return (
        f"### {row['log']}\n\n"
        f"- Display: {row['display'] or 'unknown'}\n"
        f"- Duration: {row['duration_s']:.1f} s\n"
        f"- App: {row['app_fps']:.1f} fps; presented: "
        f"{row['presented_fps']:.1f} fps; generated: "
        f"{row['generated_pct']:.0f}%\n"
        f"- Overhead: mean {row['overhead_mean']:.2f} ms, median "
        f"{row['overhead_median']:.2f} ms, max {row['overhead_max']:.2f} ms\n"
        f"- Frame p50: {row['frame_p50']:.1f} ms\n"
        f"- Present p50/p95/p99: "
        + (f"{row['present_p50']:.2f} / {row['present_p95']:.2f} / "
           f"{row['present_p99']:.2f} ms\n" if row['present_available']
           else "n/a (present_timing off)\n")
        + f"- Gate starts/stops: {row['starts']}/{row['stops']}; "
        f"backend restarts: {row['restarts']}\n"
        f"- Backend init mean: {row['init_mean']:.2f} ms; restart init mean: "
        f"{row['restart_init_mean']:.2f} ms; first-frame delay mean: "
        f"{row['first_frame_mean']:.2f} ms\n"
        f"- Path means: source {row['source_ms']:.2f} ms, generate "
        f"{row['generate_ms']:.2f} ms, present {row['present_ms']:.2f} ms\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--detail", action="store_true",
                        help="Print a detailed section per log.")
    parser.add_argument("--json", action="store_true",
                        help="Print JSON instead of Markdown.")
    parser.add_argument("--out", type=Path,
                        help="Write the report to a file.")
    args = parser.parse_args()

    reports = [parse_file(path) for path in args.logs]
    rows = [summary(report) for report in reports]
    if args.json:
        text = json.dumps(rows, indent=2) + "\n"
    else:
        parts = ["# OpenFG log summary\n", format_table(rows), ""]
        if args.detail:
            parts += [format_detail(row) for row in rows]
        text = "\n".join(parts)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
        print(f"wrote {args.out}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
