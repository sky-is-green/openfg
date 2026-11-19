# Model package contract

The app lane consumes a model package produced by the training lane. The
package is versioned, self-describing and contains a fixed test vector so the
runtime can be integrated before a model is wired into the compositor.

## Package layout

```text
model-package/
  model.onnx
  model.onnx.data          optional external weights
  config.json
  testvector/
    low.png
    high.png
    expected.png           optional model output
```

`tools/model-contract.py <package>` validates the layout, scale and image
dimensions. Add `--require-expected` once the training lane supplies a model
output.

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

## Test vector

- `low.png`: exact input frame at the render resolution.
- `high.png`: reference frame at the display resolution.
- `expected.png`: optional model output. When present, it must match the
  high-resolution dimensions; compare it with `effective` metrics in
  `scaler-lab` or the runtime test.

The fixed vector is a contract, not a benchmark. A model must reproduce its
own expected output (within tolerance) before the app lane wires it in.

## Runtime integration

1. Load `config.json`; reject unsupported scale, HDR or colour space.
2. Convert the captured low-res frame to NCHW float sRGB.
3. Run inference (IREE/SPIR-V, NCNN Vulkan or a dedicated compute path).
4. Convert back to the swapchain format and present.
5. On any error or unsupported format, fall back to EASU/RCAS or passthrough.

The app lane will publish the exact inference backend once the first package
is available.

## Acceptance

- `tools/model-contract.py --require-expected` passes.
- Runtime test vector matches `expected.png` within the agreed tolerance.
- The model card documents training data, limitations and licence.
- Latency and VRAM are measured on the target GPU.
- No game captures or checkpoints are committed to the repository.

## Versioning

A model package is immutable once published. Changes to scale, colour space,
normalization or tiling require a new package version. The app should refuse
an unknown major contract version rather than guessing.
