#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an OpenFG model package and its fixed test vector.

Expected layout (v0 accepts both names in the comments):

    model/
      model.onnx
      model.onnx.data          optional external weights
      config.json              or model-config.json
      testvector/              or test-vector/
        low.png  high.png  expected.png
        or input.npy  output.npy  expected.json

The model contract is documented in docs/MODEL-CONTRACT.md.

Example:

    python3 tools/model-contract.py /path/to/model-package
"""

import argparse
import ast
import hashlib
import json
import struct
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


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def normalize_config(raw):
    """Accept the contract layout and the v0 training package layout."""
    config = dict(raw)
    architecture = raw.get("architecture")
    if isinstance(architecture, dict):
        config.setdefault("scale", architecture.get("scale"))
        config.setdefault("channels", architecture.get("channels"))
        config.setdefault("blocks", architecture.get("blocks"))
    for key in ("input", "output"):
        value = raw.get(key)
        if isinstance(value, dict):
            layout = value.get("layout", "NCHW")
            name = value.get("name", key)
            dtype = value.get("dtype", "float32")
            config[key] = f"{name}: {layout} {dtype}"
            if key == "input":
                config["color_space"] = str(
                    value.get("color_space", "srgb")).lower()
    config.setdefault("hdr", False)
    return config


def load_config(root, errors):
    path = root / "config.json"
    if not path.is_file():
        path = root / "model-config.json"
    if not path.is_file():
        fail(errors, f"missing {root / 'config.json'} "
                     f"(or model-config.json)")
        return {}, path
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(errors, f"cannot parse {path}: {error}")
        return {}, path
    return normalize_config(raw), path


def check_image(path, errors, label):
    if not path.is_file():
        fail(errors, f"{label}: missing {path}")
        return None
    try:
        with Image.open(path) as image:
            image.verify()
        with Image.open(path) as image:
            return image.size
    except Exception as error:  # noqa: BLE001 - report the decoder error
        fail(errors, f"{label}: cannot read {path}: {error}")
        return None


def read_npy_shape(path, errors, label):
    """Read shape/dtype from a .npy header without requiring numpy."""
    try:
        with path.open("rb") as source:
            if source.read(6) != b"\x93NUMPY":
                fail(errors, f"{label}: not a .npy file")
                return None
            major, _minor = source.read(2)
            if major == 1:
                length = struct.unpack("<H", source.read(2))[0]
            else:
                length = struct.unpack("<I", source.read(4))[0]
            header = ast.literal_eval(
                source.read(length).decode("latin1"))
    except (OSError, ValueError, SyntaxError, struct.error) as error:
        fail(errors, f"{label}: cannot read {path}: {error}")
        return None
    shape = tuple(header.get("shape", ()))
    descr = header.get("descr")
    if descr not in ("<f4", "|f4", "=f4"):
        fail(errors, f"{label}: dtype must be float32 (got {descr})")
        return None
    if header.get("fortran_order"):
        fail(errors, f"{label}: Fortran-order arrays are not supported")
        return None
    return shape


def check_test_vector(root, scale, errors, warnings, require_expected):
    test = root / "testvector"
    if not test.is_dir():
        test = root / "test-vector"
    if not test.is_dir():
        fail(errors, f"missing {root / 'testvector'} (or test-vector/)")
        return

    low_png = test / "low.png"
    high_png = test / "high.png"
    input_npy = test / "input.npy"
    output_npy = test / "output.npy"
    expected_json = test / "expected.json"

    low_size = None
    high_size = None
    if low_png.is_file() or high_png.is_file():
        low_size = check_image(low_png, errors, "testvector/low.png")
        high_size = check_image(high_png, errors, "testvector/high.png")
    elif input_npy.is_file() or output_npy.is_file():
        low_shape = read_npy_shape(
            input_npy, errors, "testvector/input.npy") \
            if input_npy.is_file() else None
        high_shape = read_npy_shape(
            output_npy, errors, "testvector/output.npy") \
            if output_npy.is_file() else None
        if not input_npy.is_file():
            fail(errors, f"testvector/input.npy: missing {input_npy}")
        if not output_npy.is_file():
            fail(errors, f"testvector/output.npy: missing {output_npy}")
        if low_shape:
            if len(low_shape) != 4 or low_shape[1] != 3:
                fail(errors,
                     "testvector/input.npy must be [1,3,H,W]")
            else:
                low_size = (low_shape[3], low_shape[2])
        if high_shape:
            if len(high_shape) != 4 or high_shape[1] != 3:
                fail(errors,
                     "testvector/output.npy must be [1,3,H*scale,W*scale]")
            else:
                high_size = (high_shape[3], high_shape[2])

        if expected_json.is_file():
            try:
                expected = json.loads(
                    expected_json.read_text(encoding="utf-8"))
                for key, path in (("input", input_npy),
                                  ("output", output_npy)):
                    digest = expected.get(key, {}).get("sha256")
                    if digest and path.is_file():
                        actual = sha256(path)
                        if actual != digest:
                            fail(errors,
                                 f"{path.name}: sha256 {actual} does not "
                                 f"match expected.json")
            except (OSError, ValueError) as error:
                fail(errors, f"cannot parse {expected_json}: {error}")
        elif require_expected:
            fail(errors, "testvector/expected.json: required but missing")
    else:
        fail(errors, "testvector: no PNG or NPY pair")

    if low_size and high_size and isinstance(scale, int):
        expected = (low_size[0] * scale, low_size[1] * scale)
        if high_size != expected:
            fail(errors,
                 f"testvector high is {high_size[0]}x{high_size[1]}, "
                 f"expected {expected[0]}x{expected[1]}")

    expected_png = test / "expected.png"
    if expected_png.is_file():
        expected_size = check_image(
            expected_png, errors, "testvector/expected.png")
        if expected_size and high_size and expected_size != high_size:
            fail(errors, "testvector/expected.png must match high dimensions")
    elif require_expected and not output_npy.is_file():
        fail(errors, "testvector/expected.png or output.npy: required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--require-expected", action="store_true",
                        help="Require an expected output (PNG or NPY).")
    args = parser.parse_args()

    errors = []
    warnings = []
    root = args.package
    config, config_path = load_config(root, errors)
    for key in REQUIRED_CONFIG:
        if key not in config:
            fail(errors, f"{config_path.name}: missing '{key}'")
    scale = config.get("scale")
    if not isinstance(scale, int) or scale not in SUPPORTED_SCALES:
        fail(errors, f"{config_path.name}: scale must be 2, 3 or 4")
    if config.get("color_space") != "srgb":
        fail(errors, f"{config_path.name}: v0 requires color_space = 'srgb'")
    if config.get("hdr") is not False:
        fail(errors, f"{config_path.name}: v0 requires hdr = false")
    for key in ("input", "output"):
        value = config.get(key)
        if not isinstance(value, str) or "NCHW" not in value:
            fail(errors, f"{config_path.name}: '{key}' must describe NCHW")
    if not (root / "model.onnx").is_file():
        fail(errors, f"missing {root / 'model.onnx'}")
    if (root / "model.onnx.data").exists():
        warnings.append(
            "external ONNX weights: ship model.onnx.data with the model")

    check_test_vector(
        root, scale, errors, warnings, args.require_expected)

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
