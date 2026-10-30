# Latency

Frame generation has three different costs, and they should not be mixed:

1. **Generator cost** — copy, flow, interpolation and submit work inside the
   layer. This is the `overhead` field in the debug log (and `gpu avg` in
   `fg-lab`).
2. **Real-frame present latency** — time from the application's
   `vkQueuePresentKHR` to the presentation engine finishing the real frame.
   This is what `present_timing = true` measures with
   `VK_KHR_present_id`/`VK_KHR_present_wait`.
3. **Input-to-photon latency** — input sampling + simulation + render +
   present + scanout. A Vulkan layer cannot see the input side; that needs a
   hardware latency tester or a high-speed camera.

`present_timing` is opt-in because enabling present IDs can make the
compositor pace presents. It is a measurement mode, not a normal-play setting.

## Measured example

SpongeBob S01E01, 512x384, 23.976 fps, mpv/gpu-next Vulkan, 165 Hz display.
Same clip and settings for every row; `p50` is the real frame's
submit→present-complete time.

| generator | app fps | presented fps | overhead | present p50 |
|---|---:|---:|---:|---:|
| off (baseline) | 24.0 | 24.0 | 0.00 ms | **41.98 ms** |
| 2x flow | 24.0 | 48.0 | 0.16 ms | **41.98 ms** |
| 2x blend | 24.0 | 48.0 | 0.14 ms | **41.98 ms** |
| 2x hold | 24.0 | 48.0 | 0.15 ms | **41.98 ms** |
| 4x flow | 24.0 | 96.0 | 0.27 ms | **41.98 ms** |

At 24 fps on a 165 Hz display there are enough spare refresh slots that 2x and
4x added **no real-frame present latency** in the clean run; the generated
frames fill slots that would otherwise repeat the previous image. The layer
cost was only 0.13–0.27 ms.

That depends on headroom. With a concurrent GPU load, 4x slipped to a p50 of
~83 ms (one extra source interval) in the same clip. When the source rate
multiplied by the multiplier is at or above the refresh rate, generated frames
compete for display slots and the real frame can be delayed. Measure at the
actual game rate and load. At 2x on 165 Hz, the app cannot exceed ~82.5 fps
because only 165 presents/s exist.

## Generator compute cost

vkmark cube, 2560x1440, immediate, GPU otherwise idle. The absolute app rate
is present-bound; the useful number is the layer work per real frame.

| generator | mult | app fps | presented fps | layer work |
|---|---:|---:|---:|---:|
| off | — | 23,642 | 23,642 | 0.00 ms |
| hold | 2x | 4,028 | 8,057 | 0.19 ms |
| blend | 2x | 2,803 | 5,607 | 0.27 ms |
| flow | 2x | 1,125 | 2,250 | 0.81 ms |
| flow performance | 2x | 1,232 | 2,464 | 0.83 ms |
| hold | 4x | 1,404 | 5,615 | 0.62 ms |
| blend | 4x | 1,192 | 4,767 | 0.96 ms |
| flow | 4x | 790 | 3,156 | 1.20 ms |

## Generator quality and cost

1440p synthetic scenes, `fg-lab` GPU time and PSNR:

| generator | pan ms | pan dB | ghost dB | what it is |
|---|---:|---:|---:|---|
| flow | 1.41 | 32.83 | 26.49 | full optical flow |
| blend | 0.02 | 20.81 | 22.53 | linear A/B blend |
| hold | 0.02 | 18.86 | 20.57 | repeat the current frame |

Flow is clearly worth its cost for quality. Blend and hold are comparison
baselines, not shipping modes.

## Reproducing

```toml
[openfg]
enabled = true
generator = "flow"      # flow, blend, hold or off
present_timing = true
debug = true
```

Run the same fixed scene for each generator and read
`~/.local/state/openfg/layer.log`:

```text
[openfg] perf: app 24.0 fps, presented 48.0 fps, overhead 0.16 ms, generating yes
[openfg] present latency: p50 41.98 ms, p95 42.01 ms, p99 42.02 ms (120 samples)
```

For the layer-free generator numbers, use
`fg-lab --generator flow|blend|hold`, `tools/bench.py --generator ...` and
`tools/ghost_bench.py --generator ...`.

## What this does not measure

- Input-to-photon latency. It needs an external marker/camera or a hardware
  latency analyzer.
- GPU execution time separately from CPU submit time in the layer. `fg-lab`
  gives the clean GPU number; the layer's `overhead` includes present/copy
  submission.
