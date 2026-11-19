#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate a deterministic synthetic scaler-lab dataset.

This reuses the interpolation scene renderer in tools/testdata and writes
low/high pairs plus a manifest. It is training smoke data, not a substitute
for real game captures.

Example:

    python3 tools/scaler-lab/gen_synthetic.py \
      --out /tmp/openfg-synthetic --frames 8 --high 1280x720
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "testdata"))
from gen_frames import render  # noqa: E402

SCENES = ("pan", "occlude", "zoom", "static", "ghost")
SPLITS = {"pan": "train", "occlude": "train", "zoom": "train",
          "static": "val", "ghost": "test"}
RESAMPLE = getattr(Image, "Resampling", Image)


def parse_size(value):
    try:
        width, height = value.lower().split("x", 1)
        width, height = int(width), int(height)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("dimensions must be positive")
    return width, height


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=8,
                        help="Frames per scene.")
    parser.add_argument("--high", type=parse_size, default=(1280, 720),
                        help="High-resolution size, e.g. 1280x720.")
    parser.add_argument("--scale", type=int, choices=(2, 3, 4), default=2)
    parser.add_argument("--seed", type=int, default=1234)
    args = parser.parse_args()
    if args.frames < 1:
        raise SystemExit("--frames must be positive")
    high_w, high_h = args.high
    if high_w % args.scale or high_h % args.scale:
        raise SystemExit("--high dimensions must be divisible by --scale")
    low_w, low_h = high_w // args.scale, high_h // args.scale

    high_root = args.out / "high"
    low_root = args.out / "low"
    items = []
    for scene_index, scene in enumerate(SCENES):
        scene_high = high_root / "synthetic" / scene
        scene_low = low_root / "synthetic" / scene
        scene_high.mkdir(parents=True, exist_ok=True)
        scene_low.mkdir(parents=True, exist_ok=True)
        for frame in range(args.frames):
            t = 0.5 if args.frames == 1 else frame / (args.frames - 1)
            rng = np.random.default_rng(args.seed + scene_index)
            image = render(scene, t, high_w, high_h, rng)
            rgb = (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
            high_image = Image.fromarray(rgb, mode="RGB")
            low_image = high_image.resize((low_w, low_h), RESAMPLE.LANCZOS)
            name = f"{frame:04d}.png"
            high_image.save(scene_high / name)
            low_image.save(scene_low / name)
            items.append({
                "name": f"synthetic/{scene}/{frame:04d}",
                "game": "synthetic",
                "scene": scene,
                "split": SPLITS[scene],
                "low": f"low/synthetic/{scene}/{name}",
                "high": f"high/synthetic/{scene}/{name}",
                "fps": 60,
                "hdr": False,
            })
        print(f"{scene}: {args.frames} frame(s)")

    manifest = {
        "name": "synthetic-scaler-lab",
        "scale": args.scale,
        "high": f"{high_w}x{high_h}",
        "low": f"{low_w}x{low_h}",
        "items": items,
    }
    (args.out / "dataset.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{len(items)} pair(s), manifest {args.out / 'dataset.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
