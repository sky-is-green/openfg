#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Train a small spatial upscaler on scaler-lab pairs.

This is a scaffold for local experiments, not a finished training recipe.
Install the optional training requirements first:

    python3 -m pip install -r tools/scaler-lab/requirements-train.txt

Example:

    python3 tools/scaler-lab/train.py \
      --manifest dataset/dataset.json \
      --train-split train --val-split val \
      --patch 128 --epochs 20 --out runs/spatial-2x
"""

import argparse
import csv
import json
import math
import random
import time
from pathlib import Path

import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader

from dataset import PairDataset
from model import build_model


def charbonnier(prediction, target, epsilon=1e-3):
    return torch.sqrt((prediction - target) ** 2 + epsilon ** 2).mean()


def psnr(prediction, target):
    mse = torch.mean((prediction.clamp(0.0, 1.0) - target) ** 2)
    if mse <= 0.0:
        return math.inf
    return (10.0 * torch.log10(1.0 / mse)).item()


def evaluate(model, loader, device, amp):
    model.eval()
    total_loss = 0.0
    total_psnr = 0.0
    count = 0
    with torch.no_grad():
        for low, high in loader:
            low = low.to(device, non_blocking=True)
            high = high.to(device, non_blocking=True)
            with torch.cuda.amp.autocast(enabled=amp):
                prediction = model(low)
                loss = charbonnier(prediction, high)
            total_loss += loss.item() * low.shape[0]
            total_psnr += psnr(prediction.float(), high) * low.shape[0]
            count += low.shape[0]
    if count == 0:
        return 0.0, 0.0
    return total_loss / count, total_psnr / count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--train-split", default="train")
    parser.add_argument("--val-split", default="val")
    parser.add_argument("--scale", type=int, choices=(2, 3, 4))
    parser.add_argument("--patch", type=int, default=128)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--lr", type=float, default=2e-4)
    parser.add_argument("--channels", type=int, default=48)
    parser.add_argument("--blocks", type=int, default=6)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--limit-train", type=int, default=0)
    parser.add_argument("--limit-val", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--no-amp", action="store_true")
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--onnx", type=Path)
    parser.add_argument("--out", type=Path, default=Path("runs/spatial-2x"))
    args = parser.parse_args()

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)

    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    amp = device.type == "cuda" and not args.no_amp

    train_set = PairDataset(
        args.manifest, split=args.train_split, scale=args.scale,
        patch=args.patch, augment=True, limit=args.limit_train)
    val_set = PairDataset(
        args.manifest, split=args.val_split, scale=args.scale,
        patch=args.patch, augment=False, limit=args.limit_val)
    scale = args.scale or train_set.scale

    train_loader = DataLoader(
        train_set, batch_size=args.batch_size, shuffle=True,
        num_workers=args.workers, pin_memory=device.type == "cuda",
        drop_last=True)
    val_loader = DataLoader(
        val_set, batch_size=args.batch_size, shuffle=False,
        num_workers=args.workers, pin_memory=device.type == "cuda")

    model = build_model(scale=scale, channels=args.channels,
                        blocks=args.blocks).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=max(1, args.epochs))
    grad_scaler = torch.cuda.amp.GradScaler(enabled=amp)

    start_epoch = 0
    best_psnr = 0.0
    if args.resume:
        checkpoint = torch.load(args.resume, map_location="cpu")
        model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        start_epoch = int(checkpoint.get("epoch", 0))
        best_psnr = float(checkpoint.get("best_psnr", 0.0))

    args.out.mkdir(parents=True, exist_ok=True)
    config = {
        "manifest": str(args.manifest),
        "train_split": args.train_split,
        "val_split": args.val_split,
        "scale": scale,
        "patch": args.patch,
        "batch_size": args.batch_size,
        "epochs": args.epochs,
        "lr": args.lr,
        "channels": args.channels,
        "blocks": args.blocks,
        "model": model.config(),
    }
    (args.out / "config.json").write_text(
        json.dumps(config, indent=2) + "\n", encoding="utf-8")
    history_path = args.out / "history.csv"
    history_new = not history_path.exists()
    history = history_path.open("a", newline="", encoding="utf-8")
    writer = csv.writer(history)
    if history_new:
        writer.writerow(["epoch", "train_loss", "val_loss", "val_psnr", "seconds"])

    print(f"device {device}, amp {amp}, scale {scale}, "
          f"train {len(train_set)}, val {len(val_set)}")
    for epoch in range(start_epoch, args.epochs):
        started = time.perf_counter()
        model.train()
        total_loss = 0.0
        count = 0
        for low, high in train_loader:
            low = low.to(device, non_blocking=True)
            high = high.to(device, non_blocking=True)
            optimizer.zero_grad(set_to_none=True)
            with torch.cuda.amp.autocast(enabled=amp):
                prediction = model(low)
                loss = charbonnier(prediction, high)
            grad_scaler.scale(loss).backward()
            grad_scaler.step(optimizer)
            grad_scaler.update()
            total_loss += loss.item() * low.shape[0]
            count += low.shape[0]
        scheduler.step()
        train_loss = total_loss / max(1, count)
        val_loss, val_psnr = evaluate(model, val_loader, device, amp)
        elapsed = time.perf_counter() - started
        writer.writerow([epoch + 1, f"{train_loss:.6f}", f"{val_loss:.6f}",
                         f"{val_psnr:.4f}", f"{elapsed:.2f}"])
        history.flush()
        print(f"epoch {epoch + 1}/{args.epochs} "
              f"train {train_loss:.5f} val {val_loss:.5f} "
              f"psnr {val_psnr:.3f} dB {elapsed:.1f}s")

        checkpoint = {
            "model": model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "epoch": epoch + 1,
            "best_psnr": best_psnr,
            "config": config,
        }
        torch.save(checkpoint, args.out / "last.pt")
        if val_psnr >= best_psnr:
            best_psnr = val_psnr
            checkpoint["best_psnr"] = best_psnr
            torch.save(checkpoint, args.out / "best.pt")

    history.close()

    if args.onnx:
        checkpoint = torch.load(args.out / "best.pt", map_location="cpu")
        model.load_state_dict(checkpoint["model"])
        model.eval()
        dummy = torch.zeros(1, 3, args.patch, args.patch)
        args.onnx.parent.mkdir(parents=True, exist_ok=True)
        torch.onnx.export(
            model, dummy, args.onnx,
            input_names=["low"], output_names=["high"],
            opset_version=17,
            dynamic_axes={
                "low": {0: "batch", 2: "height", 3: "width"},
                "high": {0: "batch", 2: "height", 3: "width"},
            })
        print(f"exported {args.onnx}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
