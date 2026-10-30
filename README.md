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
- [x] GUI and per-user install scripts
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
```

Install the Qt control panel:

```sh
scripts/install-gui.sh
openfg-gui
```

The GUI edits the same config and can validate it through `openfg-cli`. It
needs PySide6 (CachyOS: `sudo pacman -S pyside6`).

For a system-wide install, use CMake directly:

```sh
sudo cmake --install build --prefix /usr/local
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

## Benchmarks

The benchmark scripts need `numpy`, `Pillow` and `scipy`
(`python3 -m pip install -r tools/requirements.txt`).

Ghosting on the `ghost` scene at 1440p (dual RX 7900 XT):

```sh
python3 tools/ghost_bench.py --scene ghost --width 2560 --height 1440
```

```
backend   full dB  edge dB  edge MAD   ghost  moving dB     ms
openfg      25.96    26.12     2.653   2.380      25.81   2.83
```

Tile-mask early-out:

```sh
python3 tools/tile_bench.py --width 2560 --height 1440
```

```
scene        off ms    on ms  speedup   off dB    on dB
pan           2.808    2.833    0.99x    31.51    31.51
zoom          2.833    2.855    0.99x    27.75    27.75
occlude       2.832    2.850    0.99x    21.16    21.16
static        2.807    0.654    4.29x    43.14    43.14
```

`ghost` (extra edges in the ground-truth edge band) is the acceptance gate for
any detail-restoration or sharpening pass: it must not increase.

## License

MIT. See `LICENSE` and `THIRD_PARTY.md`.
