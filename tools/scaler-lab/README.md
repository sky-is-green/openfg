# scaler-lab

Offline quality harness for OpenFG upscaling. It compares resampling backends
on low/high image pairs, or on synthetic pairs made by downsampling a
reference image. The Vulkan EASU/RCAS and neural backends will use the same
CSV schema.

This is a benchmarking tool, not part of the runtime layer.

## Requirements

The tools requirements already cover it:

```sh
python3 -m pip install -r tools/requirements.txt
```

`numpy`, `Pillow` and `scipy` are used for the metrics. No game assets are
included.

## Synthetic pairs

Generate a deterministic low/high dataset and manifest:

```sh
python3 tools/scaler-lab/gen_synthetic.py \
  --out /tmp/openfg-synthetic --frames 8 --high 1280x720
```

Then run a held-out scene split:

```sh
python3 tools/scaler-lab/scaler_lab.py \
  --manifest /tmp/openfg-synthetic/dataset.json --split test \
  --backend lanczos --out /tmp/scaler.csv
```

The generator reuses the `pan`, `occlude`, `zoom`, `static` and `ghost`
scenes. `static` is the validation split and `ghost` is the test split. This
is training smoke data, not a final quality verdict.

For a quick one-frame check, the older `tools/testdata/gen_frames.py` output
still works:

```sh
python3 tools/testdata/gen_frames.py --width 1280 --height 720 \
  --out /tmp/openfg-testdata
python3 tools/scaler-lab/scaler_lab.py \
  --reference /tmp/openfg-testdata --glob '*_gt.png' \
  --scale 2 --backend lanczos --out /tmp/scaler.csv
```

## Real low/high pairs

Keep the same relative paths:

```text
dataset/
  high/
    game/
      scene/
        0001.png
  low/
    game/
      scene/
        0001.png
```

```sh
python3 tools/scaler-lab/scaler_lab.py \
  --reference dataset/high --input dataset/low \
  --backend lanczos --out /tmp/real-pairs.csv
```

`--limit N` processes the first N references. `--glob` selects a subset.
The high-resolution image defines the output size; the scale column is derived
from the pair widths.

## Backends

`nearest`, `bilinear`, `bicubic`, `lanczos`. These are CPU baselines. The
Vulkan EASU/RCAS and neural backends will be added behind the same interface.

## Metrics

- PSNR on RGB.
- SSIM on luma with a Gaussian window (`sigma=1.5`).
- Optional LPIPS with `--lpips` and `--lpips-net alex|vgg`.
- CPU resize time.

The CSV contains `image`, `game`, `scene`, `split`, `hdr`, `backend`, `scale`,
`width`, `height`, `psnr`, `ssim`, `lpips` and `ms`. The console prints
per-pair rows, the mean PSNR/SSIM and a per-game/split summary. A
`*.summary.csv` file is written next to the main report. Directory mode leaves
the metadata columns empty.

Save upscaled frames for visual review with:

```sh
python3 tools/scaler-lab/scaler_lab.py   --manifest dataset/dataset.json --split test   --backend lanczos --save-upscaled /tmp/upscaled   --out /tmp/report.csv
```

Current limits:

- 8-bit sRGB images only.
- No DISTS, VMAF, temporal metrics or HDR yet.
- LPIPS needs `torch` and `lpips`; the first run may download feature weights.
- CPU timing only; no Vulkan/GPU timestamps yet.
- Validate on held-out games, not just held-out frames.

## Training (optional)

The training scaffold is a small residual CNN with a pixel-shuffle head. It is
meant for local experiments, not a finished recipe. Install the optional
dependencies:

```sh
python3 -m pip install -r tools/scaler-lab/requirements-train.txt
```

Train from a manifest:

```sh
python3 tools/scaler-lab/train.py   --manifest dataset/dataset.json   --train-split train --val-split val   --patch 128 --batch-size 8 --epochs 20   --out runs/spatial-2x
```

The run writes `config.json`, `history.csv`, `last.pt`, `best.pt` and an
optional ONNX export (`--onnx runs/spatial-2x/model.onnx`). Validation reports
PSNR; use `scaler_lab.py` for the full metric suite and held-out-game results.

Do not commit game captures or checkpoints trained on them. Public weights need
a clean data statement and a compatible license.

## Tests

```sh
python3 tools/scaler-lab/test_scaler_lab.py
```

## Data rules

Game captures stay local. Do not commit them to the repository or bundle them
in a public model release. Describe the training data in the model card and
use synthetic or permissively licensed data for public weights.
