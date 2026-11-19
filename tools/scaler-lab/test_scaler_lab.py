#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for the scaler-lab metrics and resize helpers."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

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


if __name__ == "__main__":
    unittest.main()
