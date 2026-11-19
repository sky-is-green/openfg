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
max_presented_fps = -1
pacing = "adaptive"
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
3. **`max_presented_fps`** — set `-1` to cap at the detected display refresh.
   Generation starts off and enables only when source fps × multiplier drops
   below it; the startup log should print `display: ... @ ... Hz`. Use an
   explicit value if the sandbox hides `/dev/dri/card*`, or `OPENFG_REFRESH`
   to override the detected rate.
4. **`pacing`** — `adaptive` waits for the threshold dwell; `capped` assumes
   the game itself is limited to refresh / multiplier and starts generating
   immediately. For capped, set the game's FPS limit to about 82 fps for 2x or
   41 fps for 4x on 165 Hz. The layer should log
   `start generating (capped mode, first frame, ...)` and keep `generating yes`
   while the game stays at the cap.
5. **`performance_mode`** — compare quality and frame time; it uses
   half-resolution flow (narrower refine for `flow`, half-size pyramid for
   `ffx`). At 1440p: 1.56 → 0.85 ms with flow, 0.98 → 0.49 ms with FFX.
6. **Cross-GPU** — only with two GPUs from the same vendor/driver. Set
   `cross_gpu = true` and `gpu` to the second card's PCI address.
7. **Video** — the `[video]` profile with mpv, including a scene cut.
8. **10-bit/HDR** — if available, play a 10-bit/HDR file with mpv; the log
   should say `half-float interpolation path`.

## Synthetic present benchmark

`vkmark` is a quick, deterministic check that frames are actually being
inserted. Install it (`sudo pacman -S vkmark`) and run:

```sh
vkmark -b cube:duration=8 -s 2560x1440 --present-mode immediate
```

With `debug = true`, the layer log reports app fps, presented fps (real plus
generated), overhead and the generation state. Reference RX 7900 XT numbers:

| mode | app fps | presented fps | overhead |
|---|---:|---:|---:|
| off | 22,708 | 22,708 | — |
| 2x | 1,149 | 2,306 | 0.85 ms |
| 2x performance | 1,264 | 2,536 | 0.74 ms |
| 3x | 970 | 2,883 | 1.00 ms |
| 4x | 815 | 3,265 | 1.16 ms |

The absolute app rate is present-bound at this resolution; use a real game for
frame-time impact. The value of the table is that presented/app is 2.00, 2.97
and 4.01, so the inserted frames reach the compositor.

## Tile-mask early-out

Generate the 1440p scenes once; the raw RGBA files are large and are not
committed:

```sh
python3 tools/testdata/gen_frames.py --width 2560 --height 1440
```

`tools/tile_bench.py` measures the static-tile skip on the 1440p scenes:

```sh
python3 tools/tile_bench.py --width 2560 --height 1440
```

```text
scene        off ms    on ms  speedup   off dB    on dB
pan           1.800    1.719    1.05x    32.83    32.83
zoom          1.795    1.746    1.03x    28.49    28.49
occlude       1.746    1.700    1.03x    21.30    21.30
static        1.742    0.477    3.65x    43.72    43.72
```

Moving scenes are within GPU-clock noise; the static scene skips whole 16×16
tiles and is the real early-out win.

## Generator and latency matrix

For a method comparison, set `debug = true` and `present_timing = true`, then
run the same scene with:

```toml
# Pick one per run.
generator = "off"    # layer loaded, no generation: latency baseline
# generator = "hold"   # repeat the current frame
# generator = "blend"  # linear A/B blend, no flow
# generator = "ffx"    # AMD FidelityFX optical flow (default, faster)
# generator = "flow"   # openfg optical flow (also the FFX fallback)
```

`present_timing` enables `VK_KHR_present_id`/`present_wait`; the debug log adds
real-frame present latency p50/p95/p99. It can make the compositor pace
presents, so use a fixed scene and identical settings for every row. The log’s
`app fps`, `presented fps`, `overhead` and `present latency` lines are the
matrix.

For the generator itself without the layer, `fg-lab` accepts
`--generator flow|ffx|blend|hold`; combine it with `tools/bench.py --generator`
and `tools/ghost_bench.py --generator` for quality and GPU time. `ffx` is the
FidelityFX optical-flow backend and is the one to compare against `flow` on
real games; it is the shipped default. `tools/hud_bench.py` is the targeted
regression test for the static-UI mask that removes doubled HUD text on
generated frames.

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

- The exact test: multiplier, `target_fps`, `max_presented_fps`, `pacing`,
  `performance_mode`, cross-GPU.
- Frame rate and frame time with the layer on and off.
- `OPENFG_DEBUG=1 your-game 2>openfg.log` (or the player's command) and the
  first 40 lines around startup plus any error lines.
- A screenshot or short clip if there is ghosting, tearing, a seam, a bad
  cut, or a crash. Note the timestamp and what was on screen.

## Known limitations

- Static HUD text is covered by the FFX backend's static-UI mask. Moving or
  animated UI, and all UI on the fallback `flow` backend, can still ghost.
  Ghosting is the acceptance gate; report it with the scene.
- RDNA2, RDNA1/Vega, NVIDIA and Intel are expected to meet the Vulkan
  requirements but have not been validated on real hardware here.
- Mixed-vendor cross-GPU (AMD + NVIDIA, Intel + AMD) is unsupported: the
  current timeline-semaphore handoff uses driver-private `OPAQUE_FD` handles.
- Video is limited to Vulkan-presenting players. 10-bit and HDR swapchains
  use the half-float path; HDR10 PQ is interpolated in code values, not
  linear light. Test HDR with an HDR display and an HDR file. PTS-aware
  cadence is not implemented; scene cuts are guarded by `safe = true`.
- Multiple swapchains on one device are supported (state is keyed by
  swapchain). A present queue that is not graphics/transfer-capable is still
  untested; report the application if it breaks that assumption.

## Privacy

The probe and logs contain GPU names, PCI addresses, driver versions and
process names. Remove anything you do not want public before pasting them into
an issue.

## Companion scaler

The scaler has a deterministic test path that does not need a portal or a
game:

```sh
./build/openfg-scaler --headless --source test --static \
  --input 640x360 --output 1280x720 --frames 1 \
  --dump /tmp/scale-out.rgba --dump-input /tmp/scale-in.rgba

# Compare the output with the standalone FSR1 harness
./build/scaler-gpu --input /tmp/scale-in.rgba --output /tmp/ref.rgba \
  --width 640 --height 360 --out-width 1280 --out-height 720 \
  --filter easu+rcas --iterations 1
```

The test-pattern output should match `scaler-gpu` within 1 LSB per channel on
the same input. The headless path writes raw RGBA only when `--dump` or
`--dump-input` is explicitly set; delete those files after inspection.

PipeWire capture needs a portal prompt the first time:

```sh
./build/openfg-scaler --source pipewire --capture window \
  --input 1920x1080 --crop 1920x1080+0+28 \
  --output 2560x1440 --fullscreen --sharpness 4
```

Select the game window, not the scaler. The selection is saved in
`$XDG_STATE_HOME/openfg/portal-<capture>-token` (mode 0600); the next run
restores it without a dialog. Use `--forget-token` to clear it. While
debugging, add `--windowed --no-click-through`; the fullscreen PipeWire
overlay is click-through by default and cannot be closed with Escape.

Use `--filter passthrough` with `--output` equal to the capture size for a
native reference, and `--present-timing` only for a latency measurement run,
because it serializes the loop and lowers throughput.

Do not commit game captures, portal frames or raw dumps. The scaler is a
screen-capture tool: only run the capture tests when you intend to grant the
permission, and delete any dumps after checking them.

## Neural model package

Validate a training package before wiring it in:

```sh
python3 tools/model-contract.py /path/to/model-package --require-expected
```

The validator accepts the public `config.json`/`testvector/` layout and the
v0 `model-config.json`/`test-vector/` layout, including PNG or NPY test
vectors. The current v0 package is a **2x** model: 720p -> 1440p is the
intended point; 1080p -> 1440p is 1.33x and must stay on EASU; 1080p -> 4K is
the other 2x point.

Convert the package to the optional ncnn backend:

```sh
python3 tools/model-ncnn.py /path/to/model-package \
  --onnx2ncnn /path/to/onnx2ncnn \
  --ncnnoptimize /path/to/ncnnoptimize \
  --out-dir /tmp/model-ncnn

openfg-scaler --headless --source test --filter neural \
  --model /tmp/model-ncnn/model-opt.param \
  --input 1280x720 --output 2560x1440 --frames 3
```

The ncnn backend is a validation backend and is not real-time at 720p yet.
Do not commit model weights trained on game footage; keep the package and
captures local unless the model card explicitly permits redistribution.
