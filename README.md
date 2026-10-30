# OpenFG

[![build](https://github.com/sky-is-green/openfg/actions/workflows/build.yml/badge.svg)](https://github.com/sky-is-green/openfg/actions/workflows/build.yml)

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
- [x] Adaptive target FPS and performance mode
- [x] `~/.config/openfg/conf.toml` configuration and `openfg-cli`
- [x] Process include/exclude matching (native and Wine/Proton `.exe` names)
- [x] GUI and per-user install scripts
- [x] GPU capability probe (`openfg-gpu-probe`)
- [x] Video profile with a global scene-cut guard
- [x] Video-player support notes and an mpv smoke test
- [ ] Distribution packages (Flatpak/AUR)

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
- `build/openfg-gpu-probe`

## Configuration

The layer reads `$XDG_CONFIG_HOME/openfg/conf.toml`, falling back to
`~/.config/openfg/conf.toml`. `OPENFG_CONFIG` overrides the path. Write a
starter file with:

```sh
./build/openfg-cli init
./build/openfg-cli validate
./build/openfg-cli show
./build/openfg-cli test PoE2.exe
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

# Stop generating while the measured frame rate is above this value.
# 0 = always generate. Set near the refresh rate to preserve native latency.
target_fps = 0

# Skip the full-resolution flow refinement: ~14% faster, slightly softer edges.
performance_mode = false

# Per-frame logging to stderr.
debug = false

[processes]
# Empty include list = every process. '*' and '?' wildcards are supported and
# matching is case-insensitive. Wine/Proton executables are matched by their
# .exe name.
include = []

exclude = []
```

`OPENFG_ENABLE`, `OPENFG_GPU`, `OPENFG_CROSS_GPU`, `OPENFG_MULTIPLIER`,
`OPENFG_TARGET_FPS`, `OPENFG_PERFORMANCE_MODE` and `OPENFG_DEBUG` override
individual settings for ad-hoc tests.

The layer is inactive unless `enabled = true` or `OPENFG_ENABLE=1`, and the
process matches the include/exclude lists. The file is read once per process,
early in device startup.

## Layer

Load the layer explicitly (no installation needed):

```sh
OPENFG_ENABLE=1 \
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

`OPENFG_ENABLE=1` is only needed while the config still has `enabled = false`.

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

## Limitations

- One swapchain is tracked per device. Applications with several simultaneous
  swapchains fall back to passthrough.
- The present queue and the blit commands must share a queue family. Separate
  present queue families fall back to passthrough.
- Extra swapchain images are acquired with a zero timeout, so a frame is shown
  without interpolation when no image is free. This avoids blocking the
  application's own event loop on Wayland.
- The backend's Vulkan device/instance is intentionally leaked at process exit:
  destroying it while the application device still has imports can hang RADV.
  Per-swapchain images, semaphores and command pools are still destroyed.

## Install

For a per-user install under `~/.local` (no root):

```sh
scripts/install-local.sh
```

This builds the project, installs the layer, CLI and harness, and creates a
starter config if none exists. Frame generation stays disabled until
`enabled = true` is set:

```sh
~/.local/bin/openfg-cli show
~/.local/bin/openfg-cli validate
~/.local/bin/openfg-gpu-probe
```

Install the Qt control panel:

```sh
scripts/install-gui.sh
openfg-gui
```

The GUI edits the same config and can validate or display it through
`openfg-cli`. It exposes the latency controls directly: presets (including a
4x video preset), target FPS, performance mode, and a live summary of the
effective mode. It needs PySide6 (CachyOS: `sudo pacman -S pyside6`).

## Latency

Frame generation has an inherent cost: the generated frame must be shown
**before** the current real frame, so the real frame is delayed by at least one
display refresh. The practical options are:

- `target_fps`: stop generating when the game is already near the refresh
  rate. This is the biggest win for latency at high fps. The comparison
  subtracts the layer's own per-frame copy/generate/present time from the
  measured interval to estimate the native rate, then applies a 5% hysteresis
  and a one-second dwell to avoid flapping. Generation starts off for
  `target_fps > 0`, so a game already above the target never initializes the
  backend at all; it enables only after a sustained drop. On a 165 Hz display,
  `target_fps = 120` keeps native latency above ~126 fps and only interpolates
  when the game is slower.
- `performance_mode`: skip the full-resolution flow refinement. It measured
  ~16% faster (1.58 → 1.32 ms at 1440p) with a 0.08 dB PSNR change on the pan
  scene and 0.01 dB on the ghost scene.
- `multiplier = 2` has the lowest added delay; 3x/4x insert more frames before
  the real one.
- `cross_gpu` moves interpolation to the second card. It removes GPU work from
  the render card but adds two DMA-BUF copies and cross-device semaphore
  round-trips. On the dual RX 7900 XT test system it was slower than same-GPU
  (119 vs 134 measured fps in Helldivers 2), so test it per game.

For a system-wide install, use CMake directly:

```sh
sudo cmake --install build --prefix /usr/local
```

The implicit-layer manifest is installed under
`share/vulkan/implicit_layer.d`, so after installation the layer is loaded by
every Vulkan application and the config decides whether it is active. Use
`DISABLE_OPENFG=1` to bypass it for one process.

## Video

The layer works with Vulkan-presenting video players. mpv's Vulkan output was
tested at 2x and 4x. Give the player its own profile so games keep their
settings:

```toml
[video]
processes = ["mpv"]
multiplier = 4
target_fps = 0
performance_mode = false
safe = true
```

The **Video · 4x** GUI preset fills this in. VLC 3 on this system uses
OpenGL/VAAPI and has no Vulkan output, so the layer cannot see it.

`safe = true` adds a global scene-cut guard. It reduces the flow-compensated
residual over the whole frame and shows the current frame when two frames are
unrelated, instead of blending them. On a real video cut this moved PSNR from
12.21 dB (blend) to 99 dB (current frame) while normal motion stayed at
49.21 dB. The guard costs about 0.08 ms per frame pair at 1440p. See
[docs/VIDEO.md](docs/VIDEO.md) for the mpv command, measured results and the
remaining video limitations (PTS/cadence, HDR/10-bit, subtitles).

## GPU support

Run the probe on the target machine:

```sh
./build/openfg-gpu-probe
./build/openfg-gpu-probe --gpu 0000:03:00.0
```

It prints the required extensions, features and formats, then creates a 64x64
backend context for same-GPU and cross-GPU. See
[docs/GPU-SUPPORT.md](docs/GPU-SUPPORT.md) for the RDNA2/RDNA3/NVIDIA/Intel
matrix and the mixed-vendor limitation.

## Testing

See [docs/TESTING.md](docs/TESTING.md) for the install commands, the modes to
compare, the probe/log output to include, and the current known limitations.
The most useful reports include a before/after frame rate, the GPU/driver/kernel
and a clip of any ghosting or seam.

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
byte-for-byte on the same inputs (32.7076 dB on the 1440p pan scene).

Performance mode with `--performance` at 1440p:

```
scene    normal ms   perf ms   normal dB   perf dB
pan         1.56       1.31      32.7076    32.6571
ghost       1.55       1.31      26.4529    26.4348
```

## Benchmarks

The benchmark scripts need `numpy`, `Pillow` and `scipy`
(`python3 -m pip install -r tools/requirements.txt`).

Ghosting on the `ghost` scene at 1440p (dual RX 7900 XT):

```sh
python3 tools/ghost_bench.py --scene ghost --width 2560 --height 1440
```

```
backend   full dB  edge dB  edge MAD   ghost  moving dB     ms
openfg      26.45    26.64     2.273   1.949      26.30   1.45
```

Tile-mask early-out:

```sh
python3 tools/tile_bench.py --width 2560 --height 1440
```

```
scene        off ms    on ms  speedup   off dB    on dB
pan           1.895    1.799    1.05x    32.71    32.71
zoom          1.759    1.735    1.01x    28.41    28.41
occlude       1.787    1.717    1.04x    21.29    21.29
static        1.747    0.458    3.81x    43.74    43.74
```

Pan/zoom/occlude are within run-to-run GPU clock noise; the real early-out
win is the static scene, where whole 16x16 tiles are skipped.

The interpolate pass now weights each warp by its own photo-consistency
error against the other frame (RGB and luma, ratio-clamped to keep a real
blend). The refinement pyramid now seeds each level from the filtered flow
instead of the raw field, so the median and guided-smooth passes actually
reach the interpolator. Together these moved the 1440p pan scene from 31.51
to 32.71 dB and the ghost excess from 2.380 to 1.949, while the static scene
improved to 43.74 dB.

`ghost` (extra edges in the ground-truth edge band) is the acceptance gate for
any detail-restoration or sharpening pass: it must not increase.

## License

MIT. See `LICENSE` and `THIRD_PARTY.md`.
