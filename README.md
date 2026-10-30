# openfg

Open frame generation for Linux. This is the clean-room, MIT-licensed tree:
an original optical-flow interpolator, a standalone harness, and a Vulkan layer
that is being built from scratch.

No Lossless Scaling assets, no `lsfg-vk.dll`, and no lsfg-vk source code are
used here.

## Status

- [x] `vkutil` — original Vulkan helper library
- [x] `openfg` backend — luma pyramid, block matching, LK refinement, full-res
      refinement, tile mask, optional soft occlusion
- [x] `fg-lab` — standalone 2-frame harness with GT PSNR/timing
- [x] Layer bootstrap — loader interface, instance/device/swapchain/present
      hooks; `vkcube` renders with `VK_LAYER_OPENFG_frame_generation` loaded
- [x] Same-GPU frame generation in the new layer
- [x] Cross-GPU DMA-BUF frame generation in the new layer
- [ ] Config, CLI, GUI, packaging
- [ ] Fresh public repository (new history, no private tree)

The layer now inserts one generated frame on either the same GPU or the second
GPU (DMA-BUF). It is still env-configured and only 2x; the config file, CLI,
GUI and 3x/4x loop are next.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Requirements: C++20 compiler, CMake 3.20+, Ninja, `glslc` (shaderc), Vulkan
headers (vendored), Python 3 (shader embedding).

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

## Layer

Same-GPU frame generation on the display card:

```sh
DISABLE_LSFGVK=1 \
OPENFG_ENABLE=1 OPENFG_GPU=0000:03:00.0 \
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

Cross-GPU frame generation on the second card:

```sh
DISABLE_LSFGVK=1 \
OPENFG_ENABLE=1 OPENFG_CROSS_GPU=1 OPENFG_GPU=0000:07:00.0 \
VK_LAYER_PATH="$PWD/build" \
VK_INSTANCE_LAYERS=VK_LAYER_OPENFG_frame_generation \
vkcube
```

`DISABLE_LSFGVK=1` is only needed on machines that still have the old private
layer installed; it disables that layer while testing this one.
`OPENFG_DEBUG=1` logs each generated frame.

## License

MIT. See `LICENSE` and `THIRD_PARTY.md`.
