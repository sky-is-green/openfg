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

## `.ofg` runtime file

The zero-copy Vulkan backend reads a single `model.ofg` file produced by
`tools/model-ofg.py`. It is little-endian and contains:

```text
offset 0   char[8]  "OFGNEUR1"
offset 8   u32      version = 1
offset 12  u32      scale = 2
offset 16  u32      channels
offset 20  u32      blocks
offset 24  u32      input_channels = 3
offset 28  u32      output_channels = 12   (3 * scale * scale)
offset 32  float32[]  layer weights and biases
```

Layers are stored in this order: head, then `conv1, conv2` for each residual
block, then tail. Each layer is `out * in * 3 * 3` weights in ONNX order
`[out][in][3][3]`, followed by `out` bias values. The runtime requires
`scale = 2`, 3x3 kernels, pad 1, stride 1, `channels <= 20`, and the
PixelShuffle mapping `output[c, 2h+i, 2w+j] = tail[c*4 + i*2 + j, h, w]`.
The ONNX `DepthToSpace` must use `CRD` mode (the mode PyTorch exports);
`DCR` is rejected. The format is deliberately narrow for v0; a new
architecture or scale needs a new version.

The runtime allocates two device-local activation planes, uploads the weight
buffer once and records prepare, convolution, residual-add and PixelShuffle
passes in the presenter command buffer. No activation data is read back to the
CPU. The optional ncnn `.param` path remains available for validation.

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
2. Convert the ONNX package to `model.ofg` with `tools/model-ofg.py`.
3. Run `openfg-scaler --filter neural --model <package>/model.ofg`. The
   Vulkan backend samples the captured frame, records the convolution and
   PixelShuffle passes, and leaves the result in a sampled image.
4. Present on the swapchain. No CPU readback or upload is involved.
5. On any error or unsupported format, fall back to EASU/RCAS or passthrough.

The optional ncnn backend consumes a `.param` model produced by
`tools/model-ncnn.py`; it reads the frame back through CPU memory and is a
validation path, not the production runtime. The model package and weights
stay local unless their licence explicitly allows redistribution.

## Acceptance

- `tools/model-contract.py --require-expected` passes.
- Runtime test vector matches the expected output. The CPU/ONNX contract
  tolerance is `atol=2e-3`. The zero-copy Vulkan path matched the training
  model to 1 LSB on the deterministic test pattern (mean absolute difference
  `4.7e-5` in RGBA8). The ncnn FP16 path measured max `6.0e-3`, mean
  `3.7e-4`, p99 `1.8e-3` against the ONNX output. Record the actual backend
  delta in the model card.
- The model card documents training data, limitations and licence.
- Latency and VRAM are measured on the target GPU.
- No game captures or checkpoints are committed to the repository.

## Versioning

A model package is immutable once published. Changes to scale, colour space,
normalization or tiling require a new package version. The app should refuse
an unknown major contract version rather than guessing.
