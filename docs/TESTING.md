# Testing openfg

openfg is early and the useful reports are the ones that say exactly what
hardware, driver and workload produced the result. This page is the checklist
to run before opening an issue.

## Before you start

- A Vulkan 1.2+ GPU. Run the probe first; it creates a real 64x64 backend
  context, so a failure there is the first useful error.
- Mesa/RADV or the NVIDIA/Intel driver as installed by your distro. Custom
  Vulkan layers can interact badly with other layers; test without
  `ENABLE_VK_LAYER_*` overrides if something looks wrong.
- A game or video player that presents through Vulkan. Do not stack openfg
  with DLSS Frame Generation, FSR Frame Generation or another interpolation
  layer.
- For video, mpv with `--vo=gpu-next --gpu-api=vulkan`. VLC 3 and players that
  only use OpenGL/VAAPI are not visible to a Vulkan layer.

## Build and install

```sh
git clone https://github.com/sky-is-green/openfg
cd openfg
scripts/install-local.sh
```

This builds with CMake/Ninja and installs under `~/.local`. It creates
`~/.config/openfg/conf.toml` if missing; frame generation stays disabled until
you set `enabled = true`. `scripts/uninstall-local.sh` removes the per-user
install.

Check the install:

```sh
~/.local/bin/openfg-gpu-probe
~/.local/bin/openfg-cli validate
```

## Minimal configuration

```toml
[openfg]
enabled = true
gpu = "auto"
cross_gpu = false
multiplier = 2
target_fps = 0
performance_mode = false
debug = false

[processes]
include = ["your-game.exe"]
exclude = []

[video]
processes = []
multiplier = 4
target_fps = 0
performance_mode = false
safe = true
```

Wine/Proton games are matched by their `.exe` name. `openfg-cli test
your-game.exe` prints whether the name matches and which profile applies.

## What to test

Run at least the modes that matter to you, and keep the workload identical
between them:

1. **Same GPU, 2x** — baseline. Compare frame rate and frame time with the
   layer disabled (`enabled = false` or `DISABLE_OPENFG=1`).
2. **Same GPU, 3x and 4x** — smoothness and added display delay.
3. **`target_fps`** — on a 120/144/165 Hz display, try 120. Generation starts
   off and enables only after a sustained drop.
4. **`performance_mode`** — compare quality and frame time; it skips the
   full-resolution flow refinement.
5. **Cross-GPU** — only with two GPUs from the same vendor/driver. Set
   `cross_gpu = true` and `gpu` to the second card's PCI address.
6. **Video** — the `[video]` profile with mpv, including a scene cut.
7. **10-bit/HDR** — if available, play a 10-bit/HDR file with mpv; the log
   should say `half-float interpolation path`.

## What to report

Open an issue with:

```text
GPU(s) and PCI addresses:
Driver / Mesa version:
Kernel:
Desktop / compositor:
Game or video + Proton version:
Resolution and refresh rate:
openfg-gpu-probe output:
openfg-cli show output:
```

and:

- The exact test: multiplier, `target_fps`, `performance_mode`, cross-GPU.
- Frame rate and frame time with the layer on and off.
- `OPENFG_DEBUG=1 your-game 2>openfg.log` (or the player's command) and the
  first 40 lines around startup plus any error lines.
- A screenshot or short clip if there is ghosting, tearing, a seam, a bad
  cut, or a crash. Note the timestamp and what was on screen.

## Known limitations

- Interpolation quality is close on ghosting but still behind on the 1440p
  camera-pan scene. Ghosting is the acceptance gate; report it with the scene.
- RDNA2, RDNA1/Vega, NVIDIA and Intel are expected to meet the Vulkan
  requirements but have not been validated on real hardware here.
- Mixed-vendor cross-GPU (AMD + NVIDIA, Intel + AMD) is unsupported: the
  current timeline-semaphore handoff uses driver-private `OPAQUE_FD` handles.
- Video is limited to Vulkan-presenting players. 10-bit and HDR swapchains
  use the half-float path; HDR10 PQ is interpolated in code values, not
  linear light. Test HDR with an HDR display and an HDR file. PTS-aware
  cadence is not implemented; scene cuts are guarded by `safe = true`.
- The layer currently assumes one swapchain per device and a graphics-capable
  present queue. Report the application if it breaks that assumption.

## Privacy

The probe and logs contain GPU names, PCI addresses, driver versions and
process names. Remove anything you do not want public before pasting them into
an issue.
