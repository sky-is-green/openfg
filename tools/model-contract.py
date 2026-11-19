#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an OpenFG model package and its fixed test vector.

Expected layout:

    model/
      model.onnx
      model.onnx.data          optional external weights
      config.json
      testvector/
        low.png
        high.png
        expected.png           optional model output

The model contract is documented in docs/MODEL-CONTRACT.md.

Example:

    python3 tools/model-contract.py /path/to/model-package
"""

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

REQUIRED_CONFIG = (
    "name", "version", "scale", "color_space", "input", "output", "hdr",
    "license",
)
SUPPORTED_SCALES = (2, 3, 4)


def fail(errors, message):
    errors.append(message)


def check_image(path, errors, label):
    if not path.is_file():
        fail(errors, f"{label}: missing {path}")
        return None
    try:
        with Image.open(path) as image:
            image.verify()
        with Image.open(path) as image:
            return image.size
    except Exception as error:  # noqa: BLE001 - report the actual decoder error
        fail(errors, f"{label}: cannot read {path}: {error}")
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--require-expected", action="store_true",
                        help="Require testvector/expected.png.")
    args = parser.parse_args()

    errors = []
    warnings = []
    root = args.package
    config_path = root / "config.json"
    if not config_path.is_file():
        fail(errors, f"missing {config_path}")
        config = {}
    else:
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            fail(errors, f"cannot parse {config_path}: {error}")
            config = {}

    for key in REQUIRED_CONFIG:
        if key not in config:
            fail(errors, f"config.json: missing '{key}'")
    scale = config.get("scale")
    if not isinstance(scale, int) or scale not in SUPPORTED_SCALES:
        fail(errors, "config.json: scale must be 2, 3 or 4")
    if config.get("color_space") != "srgb":
        fail(errors, "config.json: v0 requires color_space = 'srgb'")
    if config.get("hdr") is not False:
        fail(errors, "config.json: v0 requires hdr = false")
    for key in ("input", "output"):
        value = config.get(key)
        if not isinstance(value, str) or "NCHW" not in value:
            fail(errors, f"config.json: '{key}' must describe NCHW")
    if not (root / "model.onnx").is_file():
        fail(errors, f"missing {root / 'model.onnx'}")
    if (root / "model.onnx.data").exists():
        warnings.append("external ONNX weights: ship model.onnx.data with the model")

    test = root / "testvector"
    low_size = check_image(test / "low.png", errors, "testvector/low.png")
    high_size = check_image(test / "high.png", errors, "testvector/high.png")
    expected_path = test / "expected.png"
    expected_size = None
    if expected_path.is_file():
        expected_size = check_image(expected_path, errors, "testvector/expected.png")
    elif args.require_expected:
        fail(errors, "testvector/expected.png: required but missing")

    if low_size and high_size and isinstance(scale, int):
        expected = (low_size[0] * scale, low_size[1] * scale)
        if high_size != expected:
            fail(errors,
                 f"testvector/high.png is {high_size[0]}x{high_size[1]}, "
                 f"expected {expected[0]}x{expected[1]}")
    if high_size and expected_size and expected_size != high_size:
        fail(errors, "testvector/expected.png must match high.png dimensions")

    print(f"package: {root}")
    print(f"model: {config.get('name', '?')} {config.get('version', '?')}")
    for warning in warnings:
        print(f"warning: {warning}")
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("model package is valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
