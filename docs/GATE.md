# Tile-gate feature pass (Vulkan)

The EASU + neural hybrid gate consumes 31 per-tile features. The training
lane's reference implementation was CPU-only at ~510 ms/frame. This module
ports the same 31 features to Vulkan compute for the app lane.

## Inputs and outputs

- Low input: tightly packed RGBA8, 1280×720 (or another 2x source).
- EASU input: tightly packed RGBA8, 2560×1440 (exactly 2x the low input).
- Tile: 128×128 low-res / 256×256 high-res.
- Output: 31 floats per tile in `tiles.features.json` order.

The two ratio features (`easu_grad_ratio`, `easu_hf_ratio`) and `mask_frac`
are not part of the 31-feature portable gate. The logistic q0/q50 scoring is a
31-term dot product and is not part of this pass yet.

## Passes

1. Low/EASU luma.
2. Separable Gaussian blur (sigma 1.0 low, 1.5 EASU), horizontal and vertical;
   the vertical pass also writes the high-frequency map.
3. 8×8 box mean/variance maps for low-res local contrast.
4. Low Sobel/Laplacian/colour-spread/saturation maps.
5. EASU Sobel/Laplacian maps.
6. Catmull-Rom 2x luma comparison and its Sobel gradient.
7. Per-tile statistics (means, variances, fractions, anisotropy).
8. Per-tile p10/p90 percentiles with two-stage histograms.

The shader semantics follow `tools/scaler-lab/gate-cpp/gate_features.cpp`:
reflect borders for Sobel/Laplacian/Gaussian/box, clamp borders and normalized
weights for the Catmull-Rom comparison, population standard deviations and the
same 4-pixel sample grid for percentiles.

## Validation

Fixed test vector (`openfg-models/gate-v1/test-vector/`):

- Feature max abs error: `0.001754` (acceptance ≤ `0.002`)
- Feature mean abs error: `0.000078`
- gate-q0 score delta: `0.005323` (acceptance ≤ `0.01`)
- gate-q50 score delta: `0.000231`

Full 720p C++ parity (60 tiles, HD2 frame):

- Feature max abs error: `4.09e-5`
- Feature mean abs error: `3.82e-7`

The C++ reference reports `0.001733` max / `7.75e-5` mean on the same fixed
vector, so the Vulkan pass is within the reference's own parity envelope.

## Try it

```sh
openfg-gate-test \
  --low ../openfg-models/gate-v1/test-vector/low.rgba \
  --easu ../openfg-models/gate-v1/test-vector/easu.rgba \
  --width 128 --height 128 --out-width 256 --out-height 256 \
  --tile 128 --out /tmp/gate.csv

python3 tools/gate-compare.py --csv /tmp/gate.csv \
  --expected ../openfg-models/gate-v1/test-vector/expected.json \
  --gate ../openfg-models/gate-v1/gate-q0.json
```

For a full frame:

```sh
openfg-gate-test --low low.rgba --easu easu.rgba \
  --width 1280 --height 720 --out-width 2560 --out-height 1440 \
  --tile 128 --out /tmp/gate.csv
```

## Performance status

On the RX 7900 XT, a one-shot 720p/60-tile pass measured **11.5 ms GPU**
(16.6 ms wall, including pipeline/resource creation). Skipping the percentile
pass measured 5.9 ms GPU. This is a validated semantics port, not yet a
runtime-budget gate. The next optimizations are:

- run each percentile in its own workgroup to remove the serial per-tile
  histogram chain (the largest single cost);
- reduce full-frame map traffic (fuse luma/blur/maps, consider lower-precision
  intermediates after measuring gate-score error);
- cache pipelines and buffers in the scaler instead of recreating them per
  frame.

The gate model files, test vector and feature labels are game-derived and stay
local. The code is MIT.
