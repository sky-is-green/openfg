# SPDX-License-Identifier: MIT
"""Pair datasets for scaler-lab training."""

import random
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import Dataset

import scaler_lab


class PairDataset(Dataset):
    """Low/high image pairs from a scaler-lab manifest.

    The high-resolution image must be exactly ``scale`` times the low image.
    If it is not, the low image is resized with Lanczos so the pair remains
    aligned. Patches are cropped in the low-resolution grid and the same
    region is taken from the high-resolution image.
    """

    def __init__(self, manifest, split="train", scale=None, patch=128,
                 augment=True, limit=0):
        base, data = scaler_lab.load_manifest(Path(manifest))
        self.base = base
        self.manifest_scale = int(data.get("scale", scale or 2))
        self.scale = int(scale or self.manifest_scale)
        if self.scale not in (2, 3, 4):
            raise ValueError("scale must be 2, 3 or 4")
        self.patch = int(patch)
        self.augment = augment
        self.items = [
            item for item in data["items"]
            if split == "all" or item.get("split", "") == split
        ]
        if limit > 0:
            self.items = self.items[:limit]
        if not self.items:
            raise ValueError(f"no items for split {split!r}")

    def __len__(self):
        return len(self.items)

    def __getitem__(self, index):
        item = self.items[index]
        high = scaler_lab.load_image(self.base / item["high"])
        low = scaler_lab.load_image(self.base / item["low"])
        high_h, high_w = high.shape[:2]
        if high_w % self.scale or high_h % self.scale:
            raise ValueError(
                f"{item['name']}: high resolution {high_w}x{high_h} is not "
                f"divisible by {self.scale}")
        expected = (high_w // self.scale, high_h // self.scale)
        if (low.shape[1], low.shape[0]) != expected:
            low = scaler_lab.resize(low, expected, "lanczos")

        patch = min(self.patch, low.shape[1], low.shape[0])
        if patch < 8:
            raise ValueError(f"{item['name']}: image is too small for a patch")
        if self.augment:
            x = random.randint(0, low.shape[1] - patch)
            y = random.randint(0, low.shape[0] - patch)
        else:
            x = max(0, (low.shape[1] - patch) // 2)
            y = max(0, (low.shape[0] - patch) // 2)

        low_patch = low[y:y + patch, x:x + patch]
        high_patch = high[y * self.scale:(y + patch) * self.scale,
                          x * self.scale:(x + patch) * self.scale]

        if self.augment:
            if random.random() < 0.5:
                low_patch = low_patch[:, ::-1]
                high_patch = high_patch[:, ::-1]
            if random.random() < 0.5:
                low_patch = low_patch[::-1]
                high_patch = high_patch[::-1]
            if random.random() < 0.5:
                low_patch = np.transpose(low_patch, (1, 0, 2))
                high_patch = np.transpose(high_patch, (1, 0, 2))

        low_patch = np.ascontiguousarray(low_patch)
        high_patch = np.ascontiguousarray(high_patch)
        return (
            torch.from_numpy(low_patch).permute(2, 0, 1).float(),
            torch.from_numpy(high_patch).permute(2, 0, 1).float(),
        )
