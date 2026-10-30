#!/usr/bin/env python3
"""Measure the 16x16 tile-mask early-out on the synthetic scenes."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def run(repo, scene, width, height, gpu, iterations, mask):
    env = None
    if not mask:
        import os
        env = dict(os.environ)
        env["OPENFG_NO_TILE_MASK"] = "1"

    prefix = repo / "tools" / "testdata" / f"{scene}_{width}x{height}"
    outdir = Path(tempfile.gettempdir()) / "openfg"
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(repo / "build" / "fg-lab"),
        "--a", str(prefix.with_name(prefix.name + "_a.rgba")),
        "--b", str(prefix.with_name(prefix.name + "_b.rgba")),
        "--gt", str(prefix.with_name(prefix.name + "_gt.rgba")),
        "--out", str(outdir / f"{scene}_{width}x{height}_{'mask' if mask else 'nomask'}.rgba"),
        "--width", str(width), "--height", str(height),
        "--t", "0.5", "--iterations", str(iterations), "--gpu", gpu,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)

    ms = db = None
    for line in result.stdout.splitlines():
        if line.startswith("gpu min"):
            ms = float(line.split(":")[1].strip().split()[0])
        if line.startswith("psnr (RGB)"):
            db = float(line.split(":")[1].strip().split()[0])
    return ms, db


def main():
    here = Path(__file__).resolve().parent
    repo = here.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=2560)
    ap.add_argument("--height", type=int, default=1440)
    ap.add_argument("--gpu", default="0000:07:00.0")
    ap.add_argument("--iterations", type=int, default=10)
    args = ap.parse_args()

    print(f"{'scene':10} {'off ms':>8} {'on ms':>8} {'speedup':>8} "
          f"{'off dB':>8} {'on dB':>8}")
    for scene in ("pan", "zoom", "occlude", "static"):
        off_ms, off_db = run(repo, scene, args.width, args.height,
                             args.gpu, args.iterations, False)
        on_ms, on_db = run(repo, scene, args.width, args.height,
                           args.gpu, args.iterations, True)
        print(f"{scene:10} {off_ms:8.3f} {on_ms:8.3f} {off_ms / on_ms:7.2f}x "
              f"{off_db:8.2f} {on_db:8.2f}")


if __name__ == "__main__":
    main()
