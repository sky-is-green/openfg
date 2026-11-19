#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert an OpenFG model package to ncnn .param/.bin files.

ncnn is an optional system dependency. The converter needs:
  - Python onnx
  - onnx2ncnn (from ncnn tools)
  - optionally ncnnoptimize (recommended; fuses ReLU and removes splits)

The package must be a 2x residual CNN with the v0 layout:
  name, scale=2, channels, blocks, low -> high, NCHW float32 [0,1].
The proxy package uses model-config.json; the public contract uses config.json.

Example:
  python3 tools/model-ncnn.py /path/to/model-package \
      --onnx2ncnn /path/to/onnx2ncnn \
      --ncnnoptimize /path/to/ncnnoptimize \
      --out-dir /path/to/model-package/ncnn
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import onnx
    from onnx.external_data_helper import convert_model_from_external_data
except ImportError as error:
    print(f"model-ncnn: onnx is required: {error}", file=sys.stderr)
    raise SystemExit(2)


def find_tool(name, override):
    if override:
        return Path(override)
    found = shutil.which(name)
    if not found:
        raise SystemExit(
            f"model-ncnn: {name} not found; install ncnn tools or pass an "
            "explicit path")
    return Path(found)


def read_config(package):
    for name in ("config.json", "model-config.json"):
        path = package / name
        if not path.is_file():
            continue
        try:
            config = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise SystemExit(f"model-ncnn: cannot read {path}: {error}")
        if "architecture" in config:
            arch = config["architecture"]
            return {
                "name": config.get("name", package.name),
                "scale": arch.get("scale"),
                "channels": arch.get("channels"),
                "blocks": arch.get("blocks"),
                "config": name,
            }
        return {
            "name": config.get("name", package.name),
            "scale": config.get("scale"),
            "channels": config.get("channels"),
            "blocks": config.get("blocks"),
            "config": name,
        }
    raise SystemExit(
        f"model-ncnn: {package} has no config.json or model-config.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--onnx2ncnn", help="Path to onnx2ncnn")
    parser.add_argument("--ncnnoptimize", help="Path to ncnnoptimize")
    parser.add_argument("--no-optimize", action="store_true",
                        help="Skip ncnnoptimize.")
    args = parser.parse_args()

    package = args.package.resolve()
    config = read_config(package)
    if config["scale"] != 2:
        raise SystemExit("model-ncnn: v0 requires a 2x model")
    if config["channels"] is None or config["blocks"] is None:
        raise SystemExit(
            "model-ncnn: config must declare architecture channels/blocks")
    if not (package / "model.onnx").is_file():
        raise SystemExit(f"model-ncnn: missing {package / 'model.onnx'}")

    out_dir = (args.out_dir or (package / "ncnn")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    onnx2ncnn = find_tool("onnx2ncnn", args.onnx2ncnn)
    ncnnoptimize = None if args.no_optimize else find_tool(
        "ncnnoptimize", args.ncnnoptimize)

    with tempfile.TemporaryDirectory(prefix="openfg-ncnn-") as temp:
        temp = Path(temp)
        single = temp / "model.onnx"
        model = onnx.load(package / "model.onnx")
        convert_model_from_external_data(model)
        onnx.save_model(model, single)

        param = out_dir / "model.param"
        weights = out_dir / "model.bin"
        command = [str(onnx2ncnn), str(single), str(param), str(weights)]
        print("+", " ".join(command))
        result = subprocess.run(command)
        if result.returncode != 0:
            raise SystemExit(
                f"model-ncnn: onnx2ncnn failed with {result.returncode}")

        if ncnnoptimize:
            optimized_param = out_dir / "model-opt.param"
            optimized_bin = out_dir / "model-opt.bin"
            command = [str(ncnnoptimize), str(param), str(weights),
                       str(optimized_param), str(optimized_bin), "0"]
            print("+", " ".join(command))
            result = subprocess.run(command)
            if result.returncode != 0:
                raise SystemExit(
                    f"model-ncnn: ncnnoptimize failed with {result.returncode}")
            print(f"ncnn model: {optimized_param} + {optimized_bin}")
        else:
            print(f"ncnn model: {param} + {weights}")

    print(f"model: {config['name']} ({config['channels']} channels, "
          f"{config['blocks']} blocks, 2x)")
    print("run: openfg-scaler --filter neural --model "
          f"{out_dir / ('model-opt.param' if ncnnoptimize else 'model.param')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
