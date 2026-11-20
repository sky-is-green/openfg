#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert an OpenFG model package to the zero-copy Vulkan weight file.

The .ofg file is a small little-endian binary with a fixed header followed by
the float32 weights and biases of the v0 architecture:

    head conv 3 -> channels
    channels -> channels residual blocks
    tail conv channels -> 3 * scale * scale
    PixelShuffle(scale)

Layer order in the file:

    head
    for each block: conv1, conv2
    tail

Each layer stores its weights in ONNX order [out][in][3][3], followed by its
bias vector [out]. The runtime uses this order directly.

Example:

    python3 tools/model-ofg.py /path/to/model-package
    python3 tools/model-ofg.py /path/to/model-package --out /tmp/model.ofg
"""

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b"OFGNEUR1"
VERSION = 1
HEADER = struct.Struct("<8sIIIIII")


def fail(message):
    print(f"model-ofg: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_config(package):
    for name in ("config.json", "model-config.json"):
        path = package / name
        if not path.is_file():
            continue
        try:
            config = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            fail(f"cannot read {path}: {error}")
        architecture = config.get("architecture", {})
        scale = architecture.get("scale", config.get("scale"))
        channels = architecture.get("channels", config.get("channels"))
        blocks = architecture.get("blocks", config.get("blocks"))
        if not isinstance(scale, int) or scale != 2:
            fail("the Vulkan v0 runtime only supports scale = 2")
        if not isinstance(channels, int) or not 1 <= channels <= 20:
            fail("channels must be an integer from 1 to 20")
        if not isinstance(blocks, int) or not 0 <= blocks <= 64:
            fail("blocks must be an integer from 0 to 64")
        return scale, channels, blocks, name
    fail(f"{package} has no config.json or model-config.json")


def load_initializers(package):
    try:
        import onnx
        from onnx import numpy_helper
    except ImportError as error:
        fail(f"onnx is required to read the package: {error}")

    model_path = package / "model.onnx"
    if not model_path.is_file():
        fail(f"missing {model_path}")
    model = onnx.load(str(model_path))
    initializers = {}
    for tensor in model.graph.initializer:
        initializers[tensor.name] = numpy_helper.to_array(tensor)
    return model, initializers


def check_conv(model):
    """Verify the conv nodes are the plain 3x3/pad-1 convs the shader expects."""
    convs = [node for node in model.graph.node if node.op_type == "Conv"]
    for node in convs:
        attributes = {attribute.name: attribute for attribute in node.attribute}
        kernel = list(attributes["kernel_shape"].ints) \
            if "kernel_shape" in attributes else [3, 3]
        pads = list(attributes["pads"].ints) \
            if "pads" in attributes else [1, 1, 1, 1]
        strides = list(attributes["strides"].ints) \
            if "strides" in attributes else [1, 1]
        group = attributes["group"].i if "group" in attributes else 1
        if kernel != [3, 3]:
            fail(f"{node.name}: only 3x3 kernels are supported")
        if pads != [1, 1, 1, 1]:
            fail(f"{node.name}: only padding 1 is supported")
        if strides != [1, 1]:
            fail(f"{node.name}: only stride 1 is supported")
        if group != 1:
            fail(f"{node.name}: grouped convolution is not supported")
    for node in model.graph.node:
        if node.op_type != "DepthToSpace":
            continue
        attributes = {attribute.name: attribute for attribute in node.attribute}
        blocksize = attributes["blocksize"].i             if "blocksize" in attributes else 1
        mode = attributes["mode"].s if "mode" in attributes else b"DCR"
        if blocksize != 2 or mode != b"CRD":
            fail(f"{node.name}: only PixelShuffle/CRD 2x is supported")
    return convs


def tensor(initializers, name, shape):
    if name not in initializers:
        fail(f"missing initializer {name}")
    value = initializers[name]
    if list(value.shape) != shape:
        fail(f"{name} has shape {list(value.shape)}, expected {shape}")
    return value.astype("float32", copy=False).reshape(-1)


def layer(initializers, prefix, in_channels, out_channels):
    weights = tensor(
        initializers, f"{prefix}.weight",
        [out_channels, in_channels, 3, 3])
    bias = tensor(initializers, f"{prefix}.bias", [out_channels])
    return weights, bias


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--out", type=Path,
                        help="Output .ofg path (default: <package>/model.ofg).")
    args = parser.parse_args()

    package = args.package.resolve()
    scale, channels, blocks, config_name = load_config(package)
    model, initializers = load_initializers(package)
    check_conv(model)

    layers = []
    layers.append(layer(initializers, "model.head", 3, channels))
    for block in range(blocks):
        layers.append(layer(
            initializers, f"model.body.{block}.body.0", channels, channels))
        layers.append(layer(
            initializers, f"model.body.{block}.body.2", channels, channels))
    layers.append(layer(
        initializers, "model.tail", channels, 3 * scale * scale))

    output_path = args.out or (package / "model.ofg")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("wb") as output:
        output.write(HEADER.pack(
            MAGIC, VERSION, scale, channels, blocks, 3, 3 * scale * scale))
        total = 0
        for weights, bias in layers:
            output.write(weights.tobytes())
            output.write(bias.tobytes())
            total += weights.size + bias.size

    print(f"model: {config_name} ({channels} channels, {blocks} blocks, "
          f"{scale}x)")
    print(f"layers: {len(layers)}, parameters: {total}")
    print(f"output: {output_path} ({output_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
