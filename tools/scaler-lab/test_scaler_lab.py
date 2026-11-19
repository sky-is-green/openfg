#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for the scaler-lab metrics and resize helpers."""

import argparse
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scaler_lab  # noqa: E402


class MetricsTest(unittest.TestCase):
    def test_psnr_identical(self):
        image = np.zeros((8, 8, 3), dtype=np.float32)
        self.assertTrue(math.isinf(scaler_lab.psnr(image, image)))

    def test_psnr_known_mse(self):
        reference = np.zeros((8, 8, 3), dtype=np.float32)
        test = np.full((8, 8, 3), 0.1, dtype=np.float32)
        # MSE = 0.01, so PSNR = 20 dB.
        self.assertAlmostEqual(scaler_lab.psnr(reference, test), 20.0, places=5)

    def test_ssim_identical(self):
        rng = np.random.default_rng(7)
        image = rng.random((32, 32, 3), dtype=np.float32)
        self.assertAlmostEqual(scaler_lab.ssim(image, image), 1.0, places=5)

    def test_ssim_degrades(self):
        rng = np.random.default_rng(7)
        image = rng.random((32, 32, 3), dtype=np.float32)
        noisy = np.clip(image + 0.1 * rng.standard_normal(image.shape), 0, 1)
        self.assertLess(scaler_lab.ssim(image, noisy), 0.99)

    def test_resize_shape(self):
        image = np.zeros((16, 24, 3), dtype=np.float32)
        out = scaler_lab.resize(image, (48, 32), "lanczos")
        self.assertEqual(out.shape, (32, 48, 3))

    def test_resize_identity(self):
        image = np.full((8, 8, 3), 0.25, dtype=np.float32)
        out = scaler_lab.resize(image, (8, 8), "lanczos")
        np.testing.assert_allclose(out, image, atol=1e-6)

    def test_summarize(self):
        rows = [
            {"game": "a", "split": "test", "psnr": "30.0", "ssim": "0.9",
             "lpips": "0.1"},
            {"game": "a", "split": "test", "psnr": "20.0", "ssim": "0.7",
             "lpips": "0.3"},
            {"game": "b", "split": "test", "psnr": "inf", "ssim": "1.0",
             "lpips": ""},
        ]
        summary = scaler_lab.summarize(rows)
        self.assertEqual(len(summary), 2)
        first = summary[0]
        self.assertEqual(first["game"], "a")
        self.assertEqual(first["pairs"], 2)
        self.assertAlmostEqual(float(first["psnr"]), 25.0)
        self.assertAlmostEqual(float(first["ssim"]), 0.8)
        self.assertAlmostEqual(float(first["lpips"]), 0.2)

    def test_manifest_pairs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "high").mkdir()
            (root / "low").mkdir()
            Image.new("RGB", (16, 16), (200, 20, 20)).save(root / "high" / "a.png")
            Image.new("RGB", (8, 8), (20, 200, 20)).save(root / "low" / "a.png")
            (root / "dataset.json").write_text(json.dumps({
                "scale": 2,
                "items": [{
                    "name": "game/scene/0001",
                    "game": "game",
                    "scene": "scene",
                    "split": "test",
                    "low": "low/a.png",
                    "high": "high/a.png",
                }],
            }), encoding="utf-8")
            args = argparse.Namespace(
                manifest=root / "dataset.json", split="test", limit=0)
            pairs = scaler_lab.manifest_pairs(args)
            self.assertEqual(len(pairs), 1)
            self.assertEqual(pairs[0]["game"], "game")
            self.assertEqual(pairs[0]["split"], "test")
            self.assertAlmostEqual(pairs[0]["scale"], 2.0)


if __name__ == "__main__":
    unittest.main()
