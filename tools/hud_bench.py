#!/usr/bin/env python3
"""Regression test for the static-UI mask.

Builds a synthetic pair with a fast camera pan and static HUD text, then runs
the FFX interpolation twice: once with the UI mask (default) and once with
OPENFG_NO_UI_MASK=1. The measured value is the extra edge energy the ghost
copies add in a band around the text, away from the glyphs themselves. Plain
unwarped blending blurs the background but does not invent edges, so this
isolates the doubled text from the background trade.

A 48-pixel pan puts the ghost 24 pixels from the glyphs, beyond the fixed
dilation halo. That is the case the source-position mask sampling exists for.
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
    """Low-frequency background: the ghost stands out because the only sharp
    detail in the band around the text is the text itself."""
    n = background(h, w, rng)
    lum = 0.55 + 0.35 * (n[..., 1] - 0.35)
    base = np.empty((h, w, 3), dtype=np.float32)
    base[..., 0] = lum * 1.05
    base[..., 1] = lum * 0.95
    base[..., 2] = lum * 0.75
    base = gaussian_filter(base, sigma=(3, 3, 0))
    return np.clip(base, 0.0, 1.0)


def shift_mask(mask, dx):
    h, w = mask.shape
    out = np.zeros_like(mask)
    if dx >= 0:
        out[:, dx:] = mask[:, : w - dx]
    else:
        out[:, : w + dx] = mask[:, -dx:]
    return out


def overlay_text(img):
    h, w = img.shape[:2]
    im = Image.fromarray(img)
    draw = ImageDraw.Draw(im)
    for font_path in (
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    ):
        try:
            font1 = ImageFont.truetype(font_path, 38)
            font2 = ImageFont.truetype(font_path, 32)
            break
        except OSError:
            continue
    else:
        font1 = ImageFont.load_default()
        font2 = font1
    x, y = 560, 70
    draw.text((x, y), "Talk to your Guildmates", font=font1,
              fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
    box = draw.textbbox((x, y), "Talk to your Guildmates", font=font1)
    draw.text((box[2] + 60, y + 4), "0/3", font=font1,
              fill=(255, 214, 64), stroke_width=2, stroke_fill=(0, 0, 0))
    draw.text((640, y + 58), "Optional: Talk to Nata", font=font2,
              fill=(126, 205, 235), stroke_width=2, stroke_fill=(0, 0, 0))
    return np.array(im)


def write_rgba(path, rgb):
    h, w = rgb.shape[:2]
    rgba = np.dstack([rgb, np.full((h, w), 255, dtype=np.uint8)])
    rgba.tofile(path)


def run_lab(repo, frames, out, gpu, mask_on):
    cmd = [
        str(repo / "build" / "fg-lab"),
        "--a", str(frames / "a.rgba"),
        "--b", str(frames / "b.rgba"),
        "--gt", str(frames / "gt.rgba"),
        "--out", str(out),
        "--width", "1280", "--height", "720",
        "--t", "0.5", "--iterations", "10", "--gpu", gpu,
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
    data = np.fromfile(out, dtype=np.uint8).reshape(720, 1280, 4)[:, :, :3]
    return data.astype(np.float64), float(match.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gpu", default="auto")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    frames = Path(tempfile.mkdtemp(prefix="openfg-hud-"))
    rng = np.random.default_rng(1234)
    bg = (smooth_background(720, 1280, rng) * 255.0 + 0.5).astype(np.uint8)
    a = overlay_text(shift(bg, 0))
    b = overlay_text(shift(bg, -48))
    gt = overlay_text(shift(bg, -24))
    text_layer = overlay_text(np.zeros((720, 1280, 3), dtype=np.uint8))
    write_rgba(frames / "a.rgba", a)
    write_rgba(frames / "b.rgba", b)
    write_rgba(frames / "gt.rgba", gt)

    gtf = gt.astype(np.float64)
    # Ghost copies are the glyphs shifted by half the pan. Measure edge energy
    # exactly where those copies land, not over the whole background.
    text = luma(text_layer.astype(np.float64)) > 20
    ghost = ((shift_mask(text, 24) | shift_mask(text, -24))
             & ~binary_dilation(text, iterations=3))
    ref_grad = grad_mag(gtf)

    results = {}
    for name, mask_on in (("mask off", False), ("mask on", True)):
        out, psnr = run_lab(repo, frames, frames / f"{name.replace(' ', '_')}.rgba",
                            args.gpu, mask_on)
        excess = float(np.maximum(0.0, grad_mag(out) - ref_grad)[ghost].mean())
        results[name] = (psnr, excess)
        print(f"{name:9s} PSNR {psnr:7.2f} dB   halo ghost excess {excess:.3f}")

    off_psnr, off_excess = results["mask off"]
    on_psnr, on_excess = results["mask on"]
    if off_excess < 2.0:
        print("scene did not reproduce the ghost on this GPU; skipping")
        return 0
    reduction = 1.0 - on_excess / off_excess
    print(f"ghost reduction {reduction * 100:.1f}%   "
          f"PSNR change {on_psnr - off_psnr:+.2f} dB")

    ok = reduction >= 0.5 and on_psnr >= off_psnr - 0.2
    print("PASS" if ok else "FAIL")
    if not args.keep:
        for path in frames.iterdir():
            path.unlink()
        frames.rmdir()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
