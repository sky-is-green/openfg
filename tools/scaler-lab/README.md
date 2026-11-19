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

Generate a deterministic scene set with the existing frame generator:

```sh
python3 tools/testdata/gen_frames.py --width 1280 --height 720 \
  --out /tmp/openfg-testdata
```

Then downscale/upscale and report:

```sh
python3 tools/scaler-lab/scaler_lab.py \
  --reference /tmp/openfg-testdata --glob '*_gt.png' \
  --scale 2 --backend lanczos --out /tmp/scaler.csv
```

Use `--downsample box` (nearest) for an aliased source or `--downsample
lanczos` for a cleaner one. The synthetic source is intentionally simple: it
gives a fast regression signal, not a final quality verdict.

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
- CPU resize time.

The CSV contains `image`, `backend`, `scale`, `width`, `height`, `psnr`,
`ssim` and `ms`. The console prints per-pair rows and the mean PSNR/SSIM.

Current limits:

- 8-bit sRGB images only.
- No LPIPS, DISTS, VMAF, temporal metrics or HDR yet.
- CPU timing only; no Vulkan/GPU timestamps yet.
- Validate on held-out games, not just held-out frames.

## Tests

```sh
python3 tools/scaler-lab/test_scaler_lab.py
```

## Data rules

Game captures stay local. Do not commit them to the repository or bundle them
in a public model release. Describe the training data in the model card and
use synthetic or permissively licensed data for public weights.
