#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline quality harness for OpenFG upscaling.

Compares resampling backends on low/high image pairs or on synthetic pairs
made by downsampling a reference image. It reports PSNR, SSIM and CPU time.
The Vulkan/EASU/RCAS/neural backends will use the same manifest and CSV.

Examples:

    # Synthetic 2x pairs from the existing test frames
    python3 tools/scaler-lab/scaler_lab.py \
        --reference tools/testdata --glob '*_gt.png' \
        --scale 2 --backend lanczos --out /tmp/scaler.csv

    # Real low/high pairs with a manifest
    python3 tools/scaler-lab/scaler_lab.py \
        --manifest captures/dataset.json --split test \
        --backend lanczos --out /tmp/scaler.csv
"""

import argparse
import csv
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
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
GPU_BACKENDS = ("easu", "rcas", "easu+rcas")


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


def find_scaler_gpu(explicit):
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    repo = Path(__file__).resolve().parents[2]
    for candidate in (repo / "build" / "scaler-gpu",):
        if candidate.is_file():
            return candidate
    found = shutil.which("scaler-gpu")
    return Path(found) if found else None


def run_scaler_gpu(tool, low, reference, backend):
    """Run the Vulkan EASU/RCAS harness and return RGB float + GPU ms."""
    height, width = low.shape[:2]
    out_height, out_width = reference.shape[:2]
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="scaler-lab-gpu-") as directory:
        root = Path(directory)
        input_path = root / "low.rgba"
        output_path = root / "high.rgba"
        rgba = np.rint(np.clip(low, 0.0, 1.0) * 255.0).astype(np.uint8)
        rgba = np.dstack([rgba, np.full((height, width), 255, dtype=np.uint8)])
        input_path.write_bytes(rgba.tobytes())
        command = [
            str(tool),
            "--input", str(input_path),
            "--output", str(output_path),
            "--width", str(width),
            "--height", str(height),
            "--out-width", str(out_width),
            "--out-height", str(out_height),
            "--filter", backend,
            "--iterations", "3",
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(
                f"scaler-gpu failed ({result.returncode}):\n"
                f"{result.stdout}{result.stderr}")
        raw = np.frombuffer(output_path.read_bytes(), dtype=np.uint8)
        raw = raw.reshape(out_height, out_width, 4)
        image = raw[..., :3].astype(np.float32) / 255.0
        match = re.search(r"gpu avg:\s*([\d.]+)\s*ms", result.stdout)
        gpu_ms = float(match.group(1)) if match else None
    wall_ms = (time.perf_counter() - started) * 1000.0
    return image, gpu_ms, wall_ms


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


def compute_lpips(model, reference, test):
    """LPIPS for one RGB float pair. The model is created by main()."""
    import torch

    def tensor(image):
        return (torch.from_numpy(image)
                .permute(2, 0, 1).unsqueeze(0) * 2.0 - 1.0)

    device = next(model.parameters()).device
    with torch.no_grad():
        return float(model(tensor(reference).to(device),
                           tensor(test).to(device)).item())


def summarize(rows):
    """Aggregate rows by game and split for the report."""
    groups = {}
    for row in rows:
        key = (row["game"] or "all", row["split"] or "")
        groups.setdefault(key, []).append(row)
    summary = []
    for (game, split), items in sorted(groups.items()):
        psnr_values = [float(row["psnr"]) for row in items
                       if row["psnr"] != "inf"]
        lpips_values = [float(row["lpips"]) for row in items
                        if row["lpips"]]
        summary.append({
            "game": game,
            "split": split,
            "pairs": len(items),
            "psnr": (f"{sum(psnr_values) / len(psnr_values):.4f}"
                     if psnr_values else "inf"),
            "ssim": f"{sum(float(row['ssim']) for row in items) / len(items):.6f}",
            "lpips": (f"{sum(lpips_values) / len(lpips_values):.6f}"
                      if lpips_values else ""),
        })
    return summary


def find_images(root, glob):
    if root.is_file():
        return [root]
    return sorted(path for path in root.rglob(glob) if path.is_file())


def load_manifest(path):
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read manifest {path}: {error}")
    items = manifest.get("items")
    if not isinstance(items, list) or not items:
        raise SystemExit(f"{path}: 'items' must be a non-empty list")
    base = path.parent
    for item in items:
        if not isinstance(item, dict):
            raise SystemExit(f"{path}: each item must be an object")
        for key in ("name", "low", "high"):
            if not isinstance(item.get(key), str) or not item[key]:
                raise SystemExit(f"{path}: item missing string '{key}'")
    return base, manifest


def manifest_pairs(args):
    base, manifest = load_manifest(args.manifest)
    selected = []
    for item in manifest["items"]:
        split = item.get("split", "")
        if args.split != "all" and split != args.split:
            continue
        if args.limit > 0 and len(selected) >= args.limit:
            break
        low_path = base / item["low"]
        high_path = base / item["high"]
        if not low_path.is_file():
            raise SystemExit(f"missing low-resolution image: {low_path}")
        if not high_path.is_file():
            raise SystemExit(f"missing high-resolution image: {high_path}")
        low = load_image(low_path)
        high = load_image(high_path)
        scale = high.shape[1] / low.shape[1] if low.shape[1] else 0.0
        selected.append({
            "name": item["name"],
            "game": item.get("game", ""),
            "scene": item.get("scene", ""),
            "split": split,
            "hdr": bool(item.get("hdr", False)),
            "low": low,
            "reference": high,
            "scale": scale,
        })
    if not selected:
        raise SystemExit(f"{args.manifest}: no items for split {args.split!r}")
    return selected


def directory_pairs(args):
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
        name = str(reference_path.relative_to(args.reference))
        if args.input:
            if args.input.is_file():
                low_path = args.input
            else:
                low_path = args.input / reference_path.relative_to(args.reference)
            if not low_path.is_file():
                raise SystemExit(f"missing low-resolution pair: {low_path}")
            low = load_image(low_path)
            scale = width / low.shape[1] if low.shape[1] else 0.0
        else:
            low_w = max(1, round(width / args.scale))
            low_h = max(1, round(height / args.scale))
            low = resize(reference, (low_w, low_h), args.downsample)
            scale = width / low_w
        pairs.append({
            "name": name,
            "game": "",
            "scene": "",
            "split": "",
            "hdr": False,
            "low": low,
            "reference": reference,
            "scale": scale,
        })
    return pairs


def make_pairs(args):
    if args.manifest:
        return manifest_pairs(args)
    return directory_pairs(args)


def main():
    parser = argparse.ArgumentParser(
        description="Compare upscaling backends on low/high image pairs.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--reference", type=Path,
                        help="High-resolution image or directory.")
    source.add_argument("--manifest", type=Path,
                        help="JSON manifest describing low/high pairs.")
    parser.add_argument("--input", type=Path,
                        help="Low-resolution image or directory. Omit to "
                             "downsample the reference.")
    parser.add_argument("--glob", default="*.png",
                        help="Reference glob when --reference is a directory.")
    parser.add_argument("--split", choices=("all", "train", "val", "test"),
                        default="all",
                        help="Manifest split to evaluate.")
    parser.add_argument("--scale", type=float, default=2.0,
                        help="Downsample factor for synthetic pairs.")
    parser.add_argument("--backend", choices=sorted(METHODS) + list(GPU_BACKENDS),
                        default="lanczos", help="Upscaling filter.")
    parser.add_argument("--scaler-gpu", type=Path,
                        help="Path to the scaler-gpu binary for GPU backends.")
    downsamplers = sorted(set(METHODS) | {"box"})
    parser.add_argument("--downsample", choices=downsamplers, default="box",
                        help="Synthetic downsample filter. 'box' is nearest.")
    parser.add_argument("--limit", type=int, default=0,
                        help="Only process the first N pairs (0 = all).")
    parser.add_argument("--out", type=Path, default=Path("scaler-lab.csv"),
                        help="CSV report path.")
    parser.add_argument("--save-upscaled", type=Path,
                        help="Write upscaled PNGs to this directory.")
    parser.add_argument("--lpips", action="store_true",
                        help="Also compute LPIPS (requires torch and lpips).")
    parser.add_argument("--lpips-net", choices=("alex", "vgg"), default="alex",
                        help="LPIPS feature network.")
    args = parser.parse_args()
    if args.scale <= 0.0:
        raise SystemExit("--scale must be positive")
    if args.downsample == "box":
        args.downsample = "nearest"
    if args.manifest and args.input:
        raise SystemExit("--input is for --reference mode; manifests carry pairs")

    lpips_model = None
    if args.lpips:
        try:
            import lpips
            import torch
        except ImportError as error:
            raise SystemExit(
                "--lpips requires torch and lpips: "
                "python3 -m pip install -r "
                "tools/scaler-lab/requirements-train.txt") from error
        lpips_model = lpips.LPIPS(net=args.lpips_net)
        if torch.cuda.is_available():
            lpips_model = lpips_model.cuda()
        lpips_model.eval()

    gpu_tool = None
    if args.backend in GPU_BACKENDS:
        gpu_tool = find_scaler_gpu(args.scaler_gpu)
        if gpu_tool is None:
            raise SystemExit(
                "--backend easu/rcas/easu+rcas needs scaler-gpu. Build it with "
                "cmake --build build --target scaler-gpu, or pass --scaler-gpu.")

    pairs = make_pairs(args)
    rows = []
    print(f"{'image':<36} {'scale':>6} {'psnr':>8} {'ssim':>8} {'ms':>8}")
    for pair in pairs:
        reference = pair["reference"]
        if gpu_tool is not None:
            upscaled, gpu_ms, elapsed_ms = run_scaler_gpu(
                gpu_tool, pair["low"], reference, args.backend)
        else:
            started = time.perf_counter()
            upscaled = resize(pair["low"],
                              (reference.shape[1], reference.shape[0]),
                              args.backend)
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            gpu_ms = None
        value_psnr = psnr(reference, upscaled)
        value_ssim = ssim(reference, upscaled)
        value_lpips = (compute_lpips(lpips_model, reference, upscaled)
                       if lpips_model is not None else None)
        if args.save_upscaled:
            args.save_upscaled.mkdir(parents=True, exist_ok=True)
            name = pair["name"].replace("/", "_").replace("\\", "_")
            if not name.lower().endswith(".png"):
                name += ".png"
            data = np.rint(np.clip(upscaled, 0.0, 1.0) * 255.0).astype(np.uint8)
            Image.fromarray(data, mode="RGB").save(args.save_upscaled / name)
        rows.append({
            "image": pair["name"],
            "game": pair["game"],
            "scene": pair["scene"],
            "split": pair["split"],
            "hdr": "1" if pair["hdr"] else "0",
            "backend": args.backend,
            "scale": f"{pair['scale']:.3f}",
            "width": reference.shape[1],
            "height": reference.shape[0],
            "psnr": f"{value_psnr:.4f}" if math.isfinite(value_psnr) else "inf",
            "ssim": f"{value_ssim:.6f}",
            "lpips": f"{value_lpips:.6f}" if value_lpips is not None else "",
            "ms": f"{elapsed_ms:.3f}",
            "gpu_ms": f"{gpu_ms:.4f}" if gpu_ms is not None else "",
        })
        print(f"{pair['name'][:36]:<36} {pair['scale']:>6.2f} "
              f"{value_psnr:>8.3f} {value_ssim:>8.4f} {elapsed_ms:>8.2f}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    summary = summarize(rows)
    summary_path = args.out.with_name(args.out.stem + ".summary.csv")
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)

    finite = [float(row["psnr"]) for row in rows if row["psnr"] != "inf"]
    print()
    print(f"{len(rows)} pair(s), backend {args.backend}, report {args.out}")
    if finite:
        print(f"mean PSNR {sum(finite) / len(finite):.3f} dB")
    print(f"mean SSIM {sum(float(row['ssim']) for row in rows) / len(rows):.4f}")
    print()
    print(f"{'game':<24} {'split':<6} {'pairs':>5} {'psnr':>8} {'ssim':>8}")
    for row in summary:
        print(f"{row['game'][:24]:<24} {row['split']:<6} {row['pairs']:>5} "
              f"{row['psnr']:>8} {row['ssim']:>8}")
    print(f"summary {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
