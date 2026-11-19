# Model package contract

The app lane consumes a model package produced by the training lane. The
package is versioned, self-describing and contains a fixed test vector so the
runtime can be integrated before a model is wired into the compositor.

## Package layout

```text
model-package/
  model.onnx
  model.onnx.data          optional external weights
  config.json              or model-config.json
  testvector/              or test-vector/
    low.png  high.png  expected.png
    or input.npy  output.npy  expected.json
```

The v0 training packages use `model-config.json` and `test-vector/`.
`config.json` is the preferred public layout; the validator accepts both.
`tools/model-contract.py <package> --require-expected` validates the
configuration, ONNX files, hashes, scale and test-vector dimensions. The NPY
path is checked without requiring numpy.

## config.json

Required fields:

| Field | Value |
|---|---|
| `name` | Stable model name |
| `version` | Semver or date-based version |
| `scale` | `2`, `3` or `4` |
| `color_space` | `srgb` for v0 |
| `input` | Human-readable tensor description, must contain `NCHW` |
| `output` | Human-readable tensor description, must contain `NCHW` |
| `hdr` | `false` for v0 |
| `license` | Code/weight licence |

Optional fields:

| Field | Meaning |
|---|---|
| `tile` | `{ "supported": true, "min_size": N, "halo": N }` |
| `notes` | Free-form implementation notes |
| `training_data` | Short data statement, no game assets |
| `architecture` | Optional nested `{ scale, channels, blocks }` for v0 packages |

## I/O contract

- Input: NCHW RGB, float, sRGB, range `[0, 1]`.
- Output: NCHW RGB, float, sRGB, range `[0, 1]`.
- Output dimensions are exactly `input * scale`.
- The model must be fully convolutional; if tiling is not supported, 4K input
  may be split by the runtime with a suitable halo.
- HDR/10-bit is out of scope for v0. The runtime falls back to EASU/RCAS for
  HDR swapchains.
- The runtime owns colour conversion, alpha and presentation. The model only
  transforms RGB.

## Geometry

The v0 model is **2x**. The runtime requires `output = input * scale`:

- 720p -> 1440p is the intended 2x performance point.
- 1080p -> 4K is the other 2x point; the 1440p display then downscales.
- 1080p -> 1440p is 1.33x, not 2x. It stays on EASU/RCAS until a 1.33x
  model exists.

The scaler rejects a neural request whose output is not exactly 2x, rather
than silently resampling the model output.

## Test vector

- `low.png` / `input.npy`: exact input frame at the render resolution.
- `high.png` / `output.npy`: reference frame at the display resolution.
- `expected.png` or `output.npy`: model output. When present, it must match
  the high-resolution dimensions. `expected.json` may provide SHA-256 hashes
  and the reference tolerance.
- `input.npy` and `output.npy` are NCHW float32. PNGs are 8-bit previews and
  are not the exact float test data.

The fixed vector is a contract, not a benchmark. A model must reproduce its
own expected output (within tolerance) before the app lane wires it in.

## Runtime integration

1. Load `config.json`/`model-config.json`; reject unsupported scale, HDR or
   colour space.
2. Convert the captured low-res frame to the model input layout.
3. Run inference. The first backend is optional ncnn Vulkan:
   `tools/model-ncnn.py <package> --out-dir <package>/ncnn` converts the
   ONNX package to `model-opt.param`/`model-opt.bin`; run
   `openfg-scaler --filter neural --model <package>/ncnn/model-opt.param`.
4. Convert back to RGBA8 and present.
5. On any error or unsupported format, fall back to EASU/RCAS or passthrough.

The ncnn backend is a validation backend: it transfers the frame through CPU
memory and is currently slower than real time at 720p. A zero-copy Vulkan
runtime is the next performance step. The model package and weights stay
local unless their licence explicitly allows redistribution.

## Acceptance

- `tools/model-contract.py --require-expected` passes.
- Runtime test vector matches the expected output. The CPU/ONNX contract
  tolerance is `atol=2e-3`; the current ncnn FP16 path measured max
  `6.0e-3`, mean `3.7e-4`, p99 `1.8e-3` against the ONNX output. Record the
  actual backend delta in the model card.
- The model card documents training data, limitations and licence.
- Latency and VRAM are measured on the target GPU.
- No game captures or checkpoints are committed to the repository.

## Versioning

A model package is immutable once published. Changes to scale, colour space,
normalization or tiling require a new package version. The app should refuse
an unknown major contract version rather than guessing.
