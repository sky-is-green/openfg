#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline quality harness for OpenFG upscaling.

Compares resampling backends on low/high image pairs or on synthetic pairs
made by downsampling a reference image. It reports PSNR, SSIM and CPU time.
The Vulkan/EASU/RCAS/neural backends will be added on top of the same schema.

Examples:

    # Synthetic 2x pairs from the existing test frames
    python3 tools/scaler-lab/scaler_lab.py \
        --reference tools/testdata --glob '*_gt.png' \
        --scale 2 --backend lanczos --out /tmp/scaler.csv

    # Real low/high pairs, same relative paths
    python3 tools/scaler-lab/scaler_lab.py \
        --reference captures/high --input captures/low \
        --backend lanczos --out /tmp/scaler.csv
"""

import argparse
import csv
import math
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

RESAMPLE = getattr(Image, "Resampling", Image)
METHODS = {
    "nearest": RESAMPLE.NEAREST,
    "bilinear": RESAMPLE.BILINEAR,
    "bicubic": RESAMPLE.BICUBIC,
    "lanczos": RESAMPLE.LANCZOS,
}


def load_image(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def resize(image, size, method):
    """Resize an RGB float image through Pillow and return float RGB."""
    height, width = image.shape[:2]
    target_w, target_h = size
    if (width, height) == (target_w, target_h):
        return image.copy()
    data = np.rint(np.clip(image, 0.0, 1.0) * 255.0).astype(np.uint8)
    resized = Image.fromarray(data, mode="RGB").resize(
        (target_w, target_h), METHODS[method])
    return np.asarray(resized, dtype=np.float32) / 255.0


def psnr(reference, test):
    mse = float(np.mean((reference.astype(np.float64) - test.astype(np.float64)) ** 2))
    if mse <= 0.0:
        return math.inf
    return 10.0 * math.log10(1.0 / mse)


def luma(image):
    return (0.2126 * image[..., 0] +
            0.7152 * image[..., 1] +
            0.0722 * image[..., 2])


def ssim(reference, test, sigma=1.5):
    """Gaussian-window SSIM on luma; full image with reflected borders."""
    a = luma(reference)
    b = luma(test)
    mu_a = gaussian_filter(a, sigma=sigma, mode="reflect")
    mu_b = gaussian_filter(b, sigma=sigma, mode="reflect")
    var_a = gaussian_filter(a * a, sigma=sigma, mode="reflect") - mu_a * mu_a
    var_b = gaussian_filter(b * b, sigma=sigma, mode="reflect") - mu_b * mu_b
    cov = gaussian_filter(a * b, sigma=sigma, mode="reflect") - mu_a * mu_b
    c1 = 0.01 ** 2
    c2 = 0.03 ** 2
    numerator = (2.0 * mu_a * mu_b + c1) * (2.0 * cov + c2)
    denominator = (mu_a * mu_a + mu_b * mu_b + c1) * (var_a + var_b + c2)
    return float(np.mean(numerator / np.maximum(denominator, 1e-12)))


def find_images(root, glob):
    if root.is_file():
        return [root]
    return sorted(path for path in root.rglob(glob) if path.is_file())


def make_pairs(args):
    references = find_images(args.reference, args.glob)
    if not references:
        raise SystemExit(f"no images matched {args.glob!r} under {args.reference}")

    if args.limit > 0:
        references = references[:args.limit]
    if args.input and args.reference.is_file() and not args.input.is_file():
        raise SystemExit("--input must be a file when --reference is a file")

    pairs = []
    for reference_path in references:
        reference = load_image(reference_path)
        height, width = reference.shape[:2]
        if args.input:
            if args.input.is_file():
                low_path = args.input
            else:
                low_path = args.input / reference_path.relative_to(args.reference)
            if not low_path.is_file():
                raise SystemExit(f"missing low-resolution pair: {low_path}")
            low = load_image(low_path)
            low_h, low_w = low.shape[:2]
            scale = width / low_w if low_w else 0.0
            name = str(reference_path.relative_to(args.reference))
            pairs.append((name, low, reference, scale))
            continue

        low_w = max(1, round(width / args.scale))
        low_h = max(1, round(height / args.scale))
        low = resize(reference, (low_w, low_h), args.downsample)
        name = str(reference_path.relative_to(args.reference))
        pairs.append((name, low, reference, width / low_w))
    return pairs


def main():
    parser = argparse.ArgumentParser(
        description="Compare upscaling backends on low/high image pairs.")
    parser.add_argument("--reference", type=Path, required=True,
                        help="High-resolution image or directory.")
    parser.add_argument("--input", type=Path,
                        help="Low-resolution image or directory. Omit to "
                             "downsample the reference.")
    parser.add_argument("--glob", default="*.png",
                        help="Reference glob when --reference is a directory.")
    parser.add_argument("--scale", type=float, default=2.0,
                        help="Downsample factor for synthetic pairs.")
    parser.add_argument("--backend", choices=sorted(METHODS), default="lanczos",
                        help="Upscaling filter.")
    downsamplers = sorted(set(METHODS) | {"box"})
    parser.add_argument("--downsample", choices=downsamplers, default="box",
                        help="Synthetic downsample filter. 'box' is nearest.")
    parser.add_argument("--limit", type=int, default=0,
                        help="Only process the first N references (0 = all).")
    parser.add_argument("--out", type=Path, default=Path("scaler-lab.csv"),
                        help="CSV report path.")
    args = parser.parse_args()
    if args.scale <= 0.0:
        raise SystemExit("--scale must be positive")
    if args.downsample == "box":
        args.downsample = "nearest"

    pairs = make_pairs(args)
    rows = []
    print(f"{'image':<36} {'scale':>6} {'psnr':>8} {'ssim':>8} {'ms':>8}")
    for name, low, reference, scale in pairs:
        started = time.perf_counter()
        upscaled = resize(low, (reference.shape[1], reference.shape[0]),
                          args.backend)
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        value_psnr = psnr(reference, upscaled)
        value_ssim = ssim(reference, upscaled)
        rows.append({
            "image": name,
            "backend": args.backend,
            "scale": f"{scale:.3f}",
            "width": reference.shape[1],
            "height": reference.shape[0],
            "psnr": f"{value_psnr:.4f}" if math.isfinite(value_psnr) else "inf",
            "ssim": f"{value_ssim:.6f}",
            "ms": f"{elapsed_ms:.3f}",
        })
        print(f"{name[:36]:<36} {scale:>6.2f} "
              f"{value_psnr:>8.3f} {value_ssim:>8.4f} {elapsed_ms:>8.2f}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    finite = [float(row["psnr"]) for row in rows if row["psnr"] != "inf"]
    print()
    print(f"{len(rows)} pair(s), backend {args.backend}, report {args.out}")
    if finite:
        print(f"mean PSNR {sum(finite) / len(finite):.3f} dB")
    print(f"mean SSIM {sum(float(row['ssim']) for row in rows) / len(rows):.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
