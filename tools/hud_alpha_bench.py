#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression test for alpha-blended HUD text.

The opaque-text test in hud_bench.py is the easier case: the glyph core is
unchanged between frames, so a per-pixel equality test can find it. Game HUD
text is often alpha-blended and outlined, so the background behind the glyph
changes between the source frames even though the glyph itself is
screen-locked. That makes the composited pixel differ, and the old equality
test misses it. This scene reproduces that case.

Measures the extra edge energy the ghost copies add in a band around the text,
away from the glyphs themselves, with and without the static-UI mask.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import binary_dilation, gaussian_filter, sobel

sys.path.insert(0, str(Path(__file__).resolve().parent / "testdata"))
from gen_frames import background  # noqa: E402


def luma(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def grad_mag(img):
    gray = luma(img)
    return np.hypot(sobel(gray, axis=1), sobel(gray, axis=0))


def shift(img, dx):
    h, w = img.shape[:2]
    out = np.empty_like(img)
    if dx >= 0:
        out[:, dx:] = img[:, : w - dx]
        out[:, :dx] = img[:, :1]
    else:
        out[:, : w + dx] = img[:, -dx:]
        out[:, w + dx:] = img[:, -1:]
    return out


def smooth_background(h, w, rng):
    n = background(h, w, rng)
    lum = 0.55 + 0.35 * (n[..., 1] - 0.35)
    base = np.empty((h, w, 3), dtype=np.float32)
    base[..., 0] = lum * 1.05
    base[..., 1] = lum * 0.95
    base[..., 2] = lum * 0.75
    base = gaussian_filter(base, sigma=(3, 3, 0))
    return np.clip(base, 0.0, 1.0)


def text_layer(w, h, alpha, dx=0, font1_size=38, font2_size=32,
               text_x=560, text_y=70):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im)
    for font_path in (
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    ):
        try:
            font1 = ImageFont.truetype(font_path, font1_size)
            font2 = ImageFont.truetype(font_path, font2_size)
            break
        except OSError:
            continue
    else:
        font1 = ImageFont.load_default()
        font2 = font1

    x, y = text_x + dx, text_y
    fill = int(255 * alpha)
    # Outline plus fill, both alpha-blended over the moving background.
    draw.text((x, y), "Talk to your Guildmates", font=font1,
              fill=(0, 0, 0, fill), stroke_width=3,
              stroke_fill=(0, 0, 0, fill))
    draw.text((x, y), "Talk to your Guildmates", font=font1,
              fill=(255, 255, 255, fill), stroke_width=1,
              stroke_fill=(255, 255, 255, fill))
    box = draw.textbbox((x, y), "Talk to your Guildmates", font=font1)
    draw.text((box[2] + 60, y + 4), "0/3", font=font1,
              fill=(255, 214, 64, fill), stroke_width=2,
              stroke_fill=(0, 0, 0, fill))
    draw.text((x + 80, y + 58), "Optional: Talk to Nata", font=font2,
              fill=(126, 205, 235, fill), stroke_width=2,
              stroke_fill=(0, 0, 0, fill))
    return im


def composite(bg, layer):
    base = Image.fromarray((np.clip(bg, 0, 1) * 255).astype(np.uint8)).convert("RGBA")
    out = Image.alpha_composite(base, layer).convert("RGB")
    return np.asarray(out, dtype=np.float32) / 255.0


def write_rgba(path, rgb):
    h, w = rgb.shape[:2]
    rgba = np.dstack([rgb, np.full((h, w), 255, dtype=np.uint8)])
    rgba.tofile(path)


def run_lab(repo, frames, out, gpu, mask_on, width, height):
    cmd = [
        str(repo / "build" / "fg-lab"),
        "--a", str(frames / "a.rgba"),
        "--b", str(frames / "b.rgba"),
        "--gt", str(frames / "gt.rgba"),
        "--out", str(out),
        "--width", str(width),
        "--height", str(height),
        "--t", "0.5", "--iterations", "5", "--gpu", gpu,
        "--generator", "ffx", "--performance",
    ]
    env = dict(os.environ)
    env.pop("OPENFG_NO_UI_MASK", None)
    if not mask_on:
        env["OPENFG_NO_UI_MASK"] = "1"
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        raise SystemExit("fg-lab failed")
    match = re.search(r"psnr \(RGB\)\s+:\s+([\d.]+) dB", result.stdout)
    if not match:
        raise SystemExit("could not parse fg-lab output")
    data = np.fromfile(out, dtype=np.uint8).reshape(height, width, 4)[:, :, :3]
    return data.astype(np.float64), float(match.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--pan", type=int, default=48)
    ap.add_argument("--alpha", type=float, default=0.75)
    ap.add_argument("--font-size", type=int, default=38)
    ap.add_argument("--text-x", type=int, default=560)
    ap.add_argument("--text-y", type=int, default=70)
    ap.add_argument("--gpu", default="auto")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    w, h = args.width, args.height
    repo = Path(__file__).resolve().parent.parent
    frames = Path(tempfile.mkdtemp(prefix="openfg-hud-alpha-"))

    rng = np.random.default_rng(1234)
    bg = smooth_background(h, w, rng)
    layer = text_layer(
        w, h, args.alpha,
        font1_size=args.font_size,
        font2_size=max(12, int(args.font_size * 0.84)),
        text_x=args.text_x,
        text_y=args.text_y,
    )
    a = composite(shift(bg, 0), layer)
    b = composite(shift(bg, -args.pan), layer)
    gt = composite(shift(bg, -args.pan // 2), layer)
    write_rgba(frames / "a.rgba", (a * 255.0 + 0.5).astype(np.uint8))
    write_rgba(frames / "b.rgba", (b * 255.0 + 0.5).astype(np.uint8))
    write_rgba(frames / "gt.rgba", (gt * 255.0 + 0.5).astype(np.uint8))

    text = luma(np.asarray(layer, dtype=np.float32)[..., :3]) > 20
    half = args.pan // 2
    ghost = (
        (np.roll(text, half, axis=1) | np.roll(text, -half, axis=1))
        & ~binary_dilation(text, iterations=3)
    )
    ref_grad = grad_mag((gt * 255.0).astype(np.float64))

    results = {}
    for name, mask_on in (("mask off", False), ("mask on", True)):
        out = frames / f"{name.replace(' ', '_')}.rgba"
        data, psnr = run_lab(
            repo, frames, out, args.gpu, mask_on, args.width, args.height)
        excess = float(np.maximum(0.0, grad_mag(data) - ref_grad)[ghost].mean())
        results[name] = (psnr, excess)
        print(f"{name:9s} PSNR {psnr:7.2f} dB   alpha ghost excess {excess:.3f}")

        if args.keep:
            x, y, cw, ch = 520, 50, 700, 180
            crop = data[y:y + ch, x:x + cw]
            Image.fromarray(crop.astype(np.uint8)).save(
                frames / f"{name.replace(' ', '_')}.png")

    off_psnr, off_excess = results["mask off"]
    on_psnr, on_excess = results["mask on"]
    if off_excess < 2.0:
        print("scene did not reproduce the alpha ghost on this GPU; skipping")
        return 0
    reduction = 1.0 - on_excess / off_excess
    print(f"ghost reduction {reduction * 100:.1f}%   "
          f"PSNR change {on_psnr - off_psnr:+.2f} dB")

    ok = reduction >= 0.85 and on_psnr >= off_psnr - 0.2
    print("PASS" if ok else "FAIL")
    if args.keep:
        print(f"kept frames in {frames}")
    else:
        for path in frames.iterdir():
            path.unlink()
        frames.rmdir()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
