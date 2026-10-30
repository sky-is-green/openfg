#!/usr/bin/env python3
"""Generate synthetic frame pairs with ground truth for the interpolator.

Produces <name>_a.rgba, <name>_b.rgba, <name>_gt.rgba (raw RGBA8, tightly
packed) plus PNGs for eyeballing.  Every scene is rendered at 2x and box
downsampled, so the half-time frame is genuinely resolved rather than a blend.

Scenes:
  pan     camera pan + two foreground motions at different depths + static HUD
  occlude a block slides across a moving background, uncovering it
  zoom    slight zoom + pan, harder than pure translation
  static  fixed camera/background, small object motion
  ghost   fast occluder, thin geometry, transparent puff, moving HUD
"""

import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def value_noise(h, w, cell, rng):
    gh, gw = h // cell + 2, w // cell + 2
    grid = rng.random((gh, gw), dtype=np.float32)
    ys = np.linspace(0, gh - 1.001, h, dtype=np.float32)
    xs = np.linspace(0, gw - 1.001, w, dtype=np.float32)
    y0 = ys.astype(np.int32)
    x0 = xs.astype(np.int32)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    a = grid[y0][:, x0]
    b = grid[y0][:, x0 + 1]
    c = grid[y0 + 1][:, x0]
    d = grid[y0 + 1][:, x0 + 1]
    top = a * (1 - fx) + b * fx
    bot = c * (1 - fx) + d * fx
    return top * (1 - fy) + bot * fy


def background(h, w, rng):
    base = np.zeros((h, w, 3), dtype=np.float32)
    n = value_noise(h, w, 48, rng)
    n2 = value_noise(h, w, 13, rng)
    n3 = value_noise(h, w, 5, rng)
    base[..., 0] = 0.20 + 0.55 * n + 0.18 * n2
    base[..., 1] = 0.18 + 0.45 * n2 + 0.15 * n3
    base[..., 2] = 0.25 + 0.50 * n + 0.20 * n3

    yy, xx = np.mgrid[0:h, 0:w]
    grid = ((xx % 96) < 2) | ((yy % 96) < 2)
    base[grid] = base[grid] * 0.45 + 0.08

    # Fine diagonal lines: high frequency detail that exposes flow errors.
    diag = ((xx + yy) % 37) < 1
    base[diag] = base[diag] * 0.55 + 0.10
    return np.clip(base, 0.0, 1.0)


def checker(h, w, cell, c0, c1):
    yy, xx = np.mgrid[0:h, 0:w]
    m = ((xx // cell + yy // cell) % 2).astype(np.float32)
    out = np.empty((h, w, 3), dtype=np.float32)
    for i in range(3):
        out[..., i] = c0[i] * (1 - m) + c1[i] * m
    return out


def textured(h, w, seed, c0, c1):
    """Non-repeating textured surface: a real object instead of a checkerboard."""
    rng = np.random.default_rng(seed)
    n = value_noise(h, w, max(4, h // 12), rng)
    n2 = value_noise(h, w, max(2, h // 40), rng)
    m = np.clip(0.65 * n + 0.35 * n2, 0.0, 1.0)
    out = np.empty((h, w, 3), dtype=np.float32)
    for i in range(3):
        out[..., i] = c0[i] * (1 - m) + c1[i] * m
    return out


def paste(dst, src, x, y):
    h, w = src.shape[:2]
    H, W = dst.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    if x1 <= x0 or y1 <= y0:
        return
    dst[y0:y1, x0:x1] = src[y0 - y:y1 - y, x0 - x:x1 - x]


def render(scene, t, W, H, rng):
    """Render at 2x, return float RGB in [0,1] at WxH."""
    S = 2
    w, h = W * S, H * S
    bg = background(h, w, rng)

    if scene == "pan":
        shift = int(round(24 * S * t))
        bg = np.roll(bg, -shift, axis=1)
    elif scene == "zoom":
        # Simulated by scaling the background around the centre.
        scale = 1.0 + 0.02 * t
        im = Image.fromarray((bg * 255).astype(np.uint8)).resize(
            (int(w * scale), int(h * scale)), Image.BICUBIC
        )
        ox = (im.width - w) // 2
        oy = (im.height - h) // 2
        bg = np.asarray(im.crop((ox, oy, ox + w, oy + h)), dtype=np.float32) / 255.0
    elif scene == "occlude":
        shift = int(round(16 * S * t))
        bg = np.roll(bg, -shift, axis=1)
    elif scene == "ghost":
        shift = int(round(8 * S * t))
        bg = np.roll(bg, -shift, axis=1)
    # "static": background does not move at all.

    frame = bg.copy()

    if scene in ("pan", "zoom", "static"):
        # Near object, moves right. The static scene keeps the motion small so
        # most 16x16 tiles are genuinely unchanged.
        near_dx = 90 if scene in ("pan", "zoom") else 20
        far_dx = 26 if scene in ("pan", "zoom") else 6
        blk = textured(int(180 * S), int(180 * S), 11, (0.92, 0.35, 0.12), (0.10, 0.55, 0.85))
        paste(frame, blk, int((360 + near_dx * t) * S), int(300 * S))

        far = textured(int(110 * S), int(110 * S), 23, (0.15, 0.12, 0.10), (0.85, 0.86, 0.80))
        paste(frame, far, int((900 + far_dx * t) * S), int(430 * S))
    elif scene == "occlude":
        # A wide occluder sweeping over textured background.
        blk = checker(int(260 * S), int(420 * S), 30 * S, (0.95, 0.90, 0.20), (0.15, 0.10, 0.05))
        paste(frame, blk, int((-120 + 640 * t) * S), int(140 * S))
    else:
        # Ghost stress scene: a fast occluder, thin poles, a semi-transparent
        # puff, and a moving HUD. Each is a different ghosting failure mode.
        blk = textured(int(220 * S), int(220 * S), 31, (0.95, 0.45, 0.10), (0.10, 0.35, 0.80))
        paste(frame, blk, int((260 + 320 * t) * S), int(330 * S))

        # Thin vertical bars at a different speed than the background.
        bars = Image.fromarray((np.clip(frame, 0, 1) * 255).astype(np.uint8)).convert("RGBA")
        bd = ImageDraw.Draw(bars)
        for x in range(760, 1180, 34):
            bx = int((x + 18 * t) * S)
            bd.rectangle([bx, int(520 * S), bx + int(7 * S), int(760 * S)],
                         fill=(235, 235, 225, 255))
        frame = np.asarray(bars.convert("RGB"), dtype=np.float32) / 255.0

        # Semi-transparent smoke puff moving independently.
        puff = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        pd = ImageDraw.Draw(puff)
        px = int((980 + 70 * t) * S)
        for r in range(int(90 * S), 0, -int(8 * S)):
            a = int(18 * (1 - r / (90 * S)))
            pd.ellipse([px - r, int(620 * S) - r, px + r, int(620 * S) + r],
                       fill=(230, 230, 235, a))
        base = Image.fromarray((np.clip(frame, 0, 1) * 255).astype(np.uint8)).convert("RGBA")
        frame = np.asarray(Image.alpha_composite(base, puff).convert("RGB"),
                           dtype=np.float32) / 255.0

    # Static HUD: text and a bar. Rendered after everything, identical every frame.
    im = Image.fromarray((np.clip(frame, 0, 1) * 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("/usr/share/fonts/noto/NotoSans-Regular.ttf", 22 * S)
    except OSError:
        font = ImageFont.load_default()
    d.rectangle([20 * S, 20 * S, 430 * S, 60 * S], fill=(8, 8, 10))
    d.text((30 * S, 28 * S), "HUD  1440p  |  attackspeed 2.31", font=font, fill=(240, 240, 240))

    if scene == "ghost":
        # A floating combat text that moves up and right, plus a health bar
        # whose fill changes. Both are drawn into the frame, not a separate UI
        # buffer, so the layer cannot simply copy a static HUD.
        d.text((int((1040 + 40 * t) * S), int((180 - 50 * t) * S)),
               "1287", font=font, fill=(255, 90, 70))
        hp = int((0.62 - 0.10 * t) * (w - 280 * S))
        d.rectangle([w - 260 * S, h - 48 * S, w - 20 * S, h - 22 * S], fill=(10, 10, 12))
        d.rectangle([w - 255 * S, h - 44 * S, w - 255 * S + hp, h - 26 * S],
                    fill=(60, 170, 90))
    else:
        d.rectangle([w - 260 * S, h - 48 * S, w - 20 * S, h - 22 * S], fill=(10, 10, 12))
        d.text((w - 250 * S, h - 45 * S), "100 / 100", font=font, fill=(220, 230, 255))

    frame = np.asarray(im, dtype=np.float32) / 255.0
    # Box downsample 2x.
    frame = frame.reshape(H, S, W, S, 3).mean(axis=(1, 3))
    return frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    for scene in ("pan", "occlude", "zoom", "static", "ghost"):
        # Same noise seed for every time step of one scene.
        rng = np.random.default_rng(1234)
        a = render(scene, 0.0, args.width, args.height, rng)
        rng = np.random.default_rng(1234)
        gt = render(scene, 0.5, args.width, args.height, rng)
        rng = np.random.default_rng(1234)
        b = render(scene, 1.0, args.width, args.height, rng)

        for name, img in (("a", a), ("b", b), ("gt", gt)):
            rgb = (np.clip(img, 0, 1) * 255.0 + 0.5).astype(np.uint8)
            rgba = np.dstack([rgb, np.full(rgb.shape[:2], 255, dtype=np.uint8)])
            path = os.path.join(args.out, f"{scene}_{args.width}x{args.height}_{name}")
            rgba.tofile(path + ".rgba")
            Image.fromarray(rgb).save(path + ".png")
        print(f"{scene}: {args.width}x{args.height} written")


if __name__ == "__main__":
    main()
