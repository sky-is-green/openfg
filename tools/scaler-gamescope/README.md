# Gamescope scaler prototype

`openfg-scaler` is a small Gamescope wrapper for the companion-scaler backend
spike. It runs the game inside a nested Gamescope session at a lower internal
resolution and lets Gamescope upscale to the output.

This uses Gamescope's built-in **FSR1, NIS or SGSR**, not the OpenFG neural
upscaler yet. Its job is to validate the render-scale workflow, input, HDR/VRR
and capture path before the custom pass is wired in.

## Usage

```sh
tools/scaler-gamescope/openfg-scaler \
  --render 1280x720 --output 2560x1440 --filter fsr -- \
  %command%
```

For Steam, put the wrapper before `%command%` in **Properties → Launch
Options**:

```text
/path/to/openfg-scaler --render 1280x720 --filter fsr -- %command%
```

Omit `--output` to let Gamescope use the display's native size. Add
`--adaptive-sync` for VRR and `--hdr` for HDR output when the display and
compositor support them. `--mangoapp` uses Gamescope's MangoHud integration
instead of wrapping the game in `mangohud`.

## Options

| Option | Meaning |
|---|---|
| `--render WxH` | Internal render size. Default `1280x720`. |
| `--output WxH` | Output size. Omit for the display's native size. |
| `--filter NAME` | `fsr`, `nis`, `sgsr`, `linear`, `nearest` or `pixel`. |
| `--sharpness N` | Gamescope sharpness, 0..20. |
| `--refresh HZ` | Nested refresh rate. |
| `--hdr` | Enable HDR output. |
| `--adaptive-sync` | Enable adaptive sync when available. |
| `--mangoapp` | Use Gamescope's MangoHud integration. |

## Benchmark smoke

Use a deterministic Vulkan workload before a game:

```sh
# vkcube smoke; 600 frames, then exit
gamescope -W 2560 -H 1440 -w 1280 -h 720 -f -F fsr -- \
  vkcube --width 1280 --height 720 --c 600 --present_mode 2

# vkmark, if installed
gamescope -W 2560 -H 1440 -w 1280 -h 720 -f -F fsr -- \
  vkmark -s 1280x720 -b vertex:duration=20
```

Compare against a native run without `-w/-h` and `-F`. Capture frames with
OBS or `grim` for quality comparisons. `vkcube` is a pipeline smoke test;
`vkmark` is the performance benchmark.

## Test plan

1. Run a deterministic game scene at native resolution and at
   `--render 1280x720` with FSR/NIS/SGSR.
2. Compare the output at 1440p and check input, HUD text, particles and camera
   motion.
3. Record frame pacing and capture-to-present latency. Gamescope's `-T` stats
   path can log frame timings.
4. Repeat with the OpenFG layer enabled to see how upscaling interacts with
   generated frames.
5. Only then decide whether the neural upscaler belongs inside a Gamescope
   fork or in a separate PipeWire overlay.

## Caveats

- Gamescope has its own scaler options. This wrapper does not test the OpenFG
  model; it tests the workflow around it.
- A custom neural pass means forking or patching Gamescope. Verify the license
  and maintenance cost before committing.
- No game captures or game assets belong in the repository.
