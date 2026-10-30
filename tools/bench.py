#!/usr/bin/env python3
"""Run the openfg quality/latency table on the synthetic test scenes.

The frames in tools/testdata are 2560x1440. For another size, generate them
first with tools/testdata/gen_frames.py, then pass --width/--height and
--testdata.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCENES = ("pan", "zoom", "occlude", "static", "ghost")


def run_lab(repo, args, scene, performance):
    prefix = args.testdata / f"{scene}_{args.width}x{args.height}"
    cmd = [
        str(repo / "build" / "fg-lab"),
        "--a", str(prefix.with_name(prefix.name + "_a.rgba")),
        "--b", str(prefix.with_name(prefix.name + "_b.rgba")),
        "--gt", str(prefix.with_name(prefix.name + "_gt.rgba")),
        "--out", str(Path(args.outdir) / f"{scene}.rgba"),
        "--width", str(args.width),
        "--height", str(args.height),
        "--gpu", args.gpu,
        "--iterations", str(args.iterations),
    ]
    if performance:
        cmd.append("--performance")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)
    ms = re.search(r"gpu avg\s+:\s+([\d.]+) ms", result.stdout)
    psnr = re.search(r"psnr \(RGB\)\s+:\s+([\d.]+) dB", result.stdout)
    if not ms or not psnr:
        raise SystemExit(f"could not parse fg-lab output for {scene}")
    return float(ms.group(1)), float(psnr.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--width", type=int, default=2560)
    ap.add_argument("--height", type=int, default=1440)
    ap.add_argument("--gpu", default="auto")
    ap.add_argument("--iterations", type=int, default=100)
    ap.add_argument("--testdata", type=Path,
                    default=Path(__file__).resolve().parent / "testdata")
    ap.add_argument("--outdir", default="/tmp/openfg-bench")
    ap.add_argument("--ghost", action="store_true",
                    help="also run tools/ghost_bench.py")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    Path(args.outdir).mkdir(parents=True, exist_ok=True)

    print(f"{args.width}x{args.height}, {args.iterations} iterations, GPU {args.gpu}")
    print(f"{'scene':<9} {'normal ms':>10} {'perf ms':>9} "
          f"{'normal dB':>10} {'perf dB':>9}")
    for scene in SCENES:
        normal_ms, normal_db = run_lab(repo, args, scene, False)
        perf_ms, perf_db = run_lab(repo, args, scene, True)
        print(f"{scene:<9} {normal_ms:>10.3f} {perf_ms:>9.3f} "
              f"{normal_db:>10.4f} {perf_db:>9.4f}")

    if args.ghost:
        print(flush=True)
        subprocess.run([
            sys.executable, str(repo / "tools" / "ghost_bench.py"),
            "--width", str(args.width), "--height", str(args.height),
            "--gpu", args.gpu, "--iterations", str(max(10, args.iterations // 4)),
        ], check=True)


if __name__ == "__main__":
    main()
