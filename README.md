# openfg

Open frame generation for Linux. This is the clean-room, MIT-licensed tree:
an original optical-flow interpolator, a standalone harness, and a Vulkan layer.

No Lossless Scaling assets, no `lsfg-vk.dll`, and no lsfg-vk source code are
used here.

## Status

- [x] `vkutil` — original Vulkan helper library
- [x] `openfg` backend — luma pyramid, block matching, LK refinement, full-res
      refinement, tile mask, optional soft occlusion
- [x] `fg-lab` — standalone 2-frame harness with GT PSNR/timing
- [x] Layer bootstrap — loader interface, instance/device/swapchain/present
      hooks
- [x] Same-GPU frame generation (OpaqueFd)
- [x] Cross-GPU frame generation (DMA-BUF)
- [x] 2x, 3x and 4x frame insertion
- [x] `~/.config/openfg/conf.toml` configuration and `openfg-cli`
- [x] Process include/exclude matching (native and Wine/Proton `.exe` names)
- [ ] GUI and distribution packages

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Requirements: C++20 compiler, CMake 3.20+, Ninja, `glslc` (shaderc), Vulkan
headers (vendored), Python 3 (shader embedding).

The build produces:

- `build/libopenfg-layer.so` and `VkLayer_OPENFG_frame_generation.json`
- `build/openfg-cli`
- `build/fg-lab`

## Configuration

The layer reads `$XDG_CONFIG_HOME/openfg/conf.toml`, falling back to
`~/.config/openfg/conf.toml`. `OPENFG_CONFIG` overrides the path. Write a
starter file with:

```sh
./build/openfg-cli init
./build/openfg-cli validate
./build/openfg-cli show
```

Example:

```toml
[openfg]
enabled = true

# "auto" uses the application's display GPU. For cross-GPU, give the second
# card's PCI address (or a device name / "vendor:device").
gpu = "auto"

# true: interpolate on `gpu` and share frames over DMA-BUF.
# false: interpolate on the display GPU with OpaqueFd.
cross_gpu = false

# Generated frames per real frame: 2, 3 or 4.
multiplier = 2

# Per-frame logging to stderr.
debug = false

[processes]
# Empty include list = every process. '*' and '?' wildcards are supported and
# matching is case-insensitive. Wine/Proton executables are matched by their
# .exe name.
include = []

exclude = []
```

`OPENFG_ENABLE`, `OPENFG_GPU`, `OPENFG_CROSS_GPU`, `OPENFG_MULTIPLIER` and
`OPENFG_DEBUG` override individual settings for ad-hoc tests.

The layer is inactive unless `enabled = true` or `OPENFG_ENABLE=1`. The file is
read once per process, early in device startup.

## Layer

Load the layer explicitly (no installation needed):

```sh
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

Make sure no other frame-generation layer is active; two layers inserting
frames into the same swapchain will fight. Disable any other frame-generation
layer with its own disable environment variable if necessary.

Same-GPU, 4x, with debug logging:

```sh
OPENFG_ENABLE=1 OPENFG_MULTIPLIER=4 OPENFG_DEBUG=1 \
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

Cross-GPU on the second card:

```sh
OPENFG_ENABLE=1 OPENFG_CROSS_GPU=1 OPENFG_GPU=0000:07:00.0 \
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

For a system-wide install:

```sh
sudo cmake --install build --prefix /usr/local
sudo ldconfig
```

The implicit-layer manifest is installed under
`share/vulkan/implicit_layer.d`, so after installation the layer is loaded by
every Vulkan application and the config decides whether it is active. Use
`DISABLE_OPENFG=1` to bypass it for one process.

## Harness

Generate the synthetic frames once:

```sh
python3 tools/testdata/gen_frames.py --width 2560 --height 1440
```

Run the interpolator:

```sh
./build/fg-lab \
  --a tools/testdata/pan_2560x1440_a.rgba \
  --b tools/testdata/pan_2560x1440_b.rgba \
  --gt tools/testdata/pan_2560x1440_gt.rgba \
  --out /tmp/out.rgba --width 2560 --height 1440 --t 0.5 \
  --gpu 0000:07:00.0
```

The backend reproduces the reference implementation used during development
byte-for-byte on the same inputs (31.5095 dB on the 1440p pan scene).

## License

MIT. See `LICENSE` and `THIRD_PARTY.md`.
