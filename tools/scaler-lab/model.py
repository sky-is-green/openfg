# SPDX-License-Identifier: MIT
"""Small spatial upscaling models for scaler-lab training."""

import torch
from torch import nn


class ResidualBlock(nn.Module):
    def __init__(self, channels):
        super().__init__()
        self.body = nn.Sequential(
            nn.Conv2d(channels, channels, 3, padding=1),
            nn.ReLU(inplace=True),
            nn.Conv2d(channels, channels, 3, padding=1),
        )

    def forward(self, x):
        return x + self.body(x)


class SpatialScaler(nn.Module):
    """Residual CNN with a pixel-shuffle head.

    This is deliberately small: the first target is real-time 1080p -> 1440p
    on a single RX 7900 XT, not a research-scale super-resolution network.
    """

    def __init__(self, scale=2, channels=48, blocks=6):
        super().__init__()
        if scale not in (2, 3, 4):
            raise ValueError("scale must be 2, 3 or 4")
        self.scale = scale
        self.head = nn.Conv2d(3, channels, 3, padding=1)
        self.body = nn.Sequential(*[ResidualBlock(channels) for _ in range(blocks)])
        self.tail = nn.Conv2d(channels, 3 * scale * scale, 3, padding=1)
        self.shuffle = nn.PixelShuffle(scale)

    def forward(self, x):
        x = self.head(x)
        x = self.body(x)
        x = self.tail(x)
        return self.shuffle(x)

    def config(self):
        return {
            "scale": self.scale,
            "channels": self.head.out_channels,
            "blocks": len(self.body),
        }


def build_model(scale=2, channels=48, blocks=6):
    return SpatialScaler(scale=scale, channels=channels, blocks=blocks)
