#!/usr/bin/env python3
"""Ghosting benchmark for frame generation.

Compares a generated half-time frame against the ground truth on a scene with
a fast occluder, thin geometry, a transparent puff and a moving HUD.

Metrics:
  full PSNR   - whole frame, RGB
  edge PSNR   - PSNR in a dilated band around ground-truth edges (ghost doubles
                and trailing edges show up here much more than in full PSNR)
  edge MAD    - mean absolute RGB error in that band
  ghost excess- mean of max(0, |grad(C)| - |grad(GT)|) in the band; extra edges
                created by ghosting increase this
"""

import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import binary_dilation, sobel


def read_rgba(path, width, height):
    data = np.fromfile(path, dtype=np.uint8)
    expected = width * height * 4
    if data.size != expected:
        raise SystemExit(f"{path}: expected {expected} bytes, got {data.size}")
    return data.reshape(height, width, 4)[:, :, :3].astype(np.float64)


def luma(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def grad_mag(img):
    gray = luma(img)
    return np.hypot(sobel(gray, axis=1), sobel(gray, axis=0))


def psnr(a, b, mask=None):
    diff = (a - b) ** 2
    if mask is not None:
        diff = diff[mask]
    mse = float(np.mean(diff))
    return 99.0 if mse <= 1e-12 else 10.0 * math.log10((255.0 * 255.0) / mse)


def metrics(c, gt, a, b):
    edge = grad_mag(gt) > 0.06
    edge = binary_dilation(edge, iterations=2)
    diff = np.abs(c - gt).mean(axis=2)
    excess = np.maximum(0.0, grad_mag(c) - grad_mag(gt))
    moving = np.max(np.abs(a - b), axis=2) > 16.0
    return {
        "full": psnr(c, gt),
        "edge": psnr(c, gt, edge),
        "edge_mad": float(diff[edge].mean()),
        "ghost": float(excess[edge].mean()),
        "moving": psnr(c, gt, moving),
    }


def run_openfg(repo, scene, width, height, gpu, iterations, generator="flow"):
    prefix = repo / "tools" / "testdata" / f"{scene}_{width}x{height}"
    outdir = Path(tempfile.gettempdir()) / "openfg"
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / f"{scene}_{width}x{height}_openfg.rgba"
    cmd = [
        str(repo / "build" / "fg-lab"),
        "--a", str(prefix.with_name(prefix.name + "_a.rgba")),
        "--b", str(prefix.with_name(prefix.name + "_b.rgba")),
        "--gt", str(prefix.with_name(prefix.name + "_gt.rgba")),
        "--out", str(out),
        "--width", str(width), "--height", str(height),
        "--t", "0.5", "--iterations", str(iterations), "--gpu", gpu,
    ]
    if generator != "flow":
        cmd += ["--generator", generator]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)
    ms = None
    for line in result.stdout.splitlines():
        if line.startswith("gpu min"):
            ms = float(line.split(":")[1].strip().split()[0])
    return out, ms


def main():
    here = Path(__file__).resolve().parent
    repo = here.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="ghost")
    ap.add_argument("--width", type=int, default=2560)
    ap.add_argument("--height", type=int, default=1440)
    ap.add_argument("--gpu", default="0000:07:00.0")
    ap.add_argument("--iterations", type=int, default=10)
    ap.add_argument("--generator", choices=("flow", "blend", "hold"),
                    default="flow")
    args = ap.parse_args()

    prefix = repo / "tools" / "testdata" / f"{args.scene}_{args.width}x{args.height}"
    a = read_rgba(prefix.with_name(prefix.name + "_a.rgba"), args.width, args.height)
    b = read_rgba(prefix.with_name(prefix.name + "_b.rgba"), args.width, args.height)
    gt = read_rgba(prefix.with_name(prefix.name + "_gt.rgba"), args.width, args.height)

    openfg_path, ms = run_openfg(repo, args.scene, args.width, args.height,
                                 args.gpu, args.iterations, args.generator)
    rows = [("openfg", openfg_path, ms)]

    print(f"{'backend':8} {'full dB':>8} {'edge dB':>8} {'edge MAD':>9} "
          f"{'ghost':>7} {'moving dB':>10} {'ms':>6}")
    images = []
    for name, path, ms in rows:
        c = read_rgba(path, args.width, args.height)
        m = metrics(c, gt, a, b)
        print(f"{name:8} {m['full']:8.2f} {m['edge']:8.2f} {m['edge_mad']:9.3f} "
              f"{m['ghost']:7.3f} {m['moving']:10.2f} "
              f"{ms if ms is not None else float('nan'):6.2f}")
        images.append(c)

    # Save a crop around the fast occluder and HUD for eyeballing.
    crop = (slice(300, 820), slice(220, 1250))
    rows_img = [a[crop], b[crop], gt[crop]] + [c[crop] for c in images]
    outdir = Path(tempfile.gettempdir()) / "openfg"
    outdir.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.concatenate(rows_img, axis=0).astype(np.uint8)).save(
        outdir / f"{args.scene}_ghost_compare.png")


if __name__ == "__main__":
    main()
