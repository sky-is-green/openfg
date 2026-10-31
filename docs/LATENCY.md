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

The same test at 60 fps shows the boundary:

| source | mode | app fps | presented fps | present p50 | present p95 |
|---:|---|---:|---:|---:|---:|
| 60 | off | 60.2 | 60.2 | 16.98 ms | 17.00 ms |
| 60 | 2x flow | 60.1 | 120.2 | 16.99 ms | 32.98 ms |
| 60 | 4x flow | 60.1 | 165.8 | 49.99 ms | 50.00 ms |

At 24 fps there are enough spare refresh slots that 2x and 4x added **no**
real-frame present latency; at 60 fps, 2x keeps the p50 but some frames hit
two intervals (p95 33 ms), and 4x wants 240 presents/s on a 165 Hz display, so
the real frame lands about three source intervals later (p50 50 ms).

The rule of thumb is **source rate × multiplier ≤ display refresh**. For
2x on 165 Hz that means a source cap around 82, not 120; for 4x it means
around 41. `max_presented_fps = -1` derives that cap from the detected display
refresh automatically. `target_fps` remains a manual source cap; when both are
set, the stricter one wins.

## Pacing modes

`pacing = "adaptive"` is the default. It uses the measured-rate threshold and
a one-second dwell in both directions. `pacing = "capped"` assumes the
application itself is capped at `refresh / multiplier`. It starts generating
on the first frame below the target, with only the over-refresh stop as a
safety. A variable-rate 30↔90 fps test with a 60 fps target measured:

| event | adaptive | capped |
|---|---:|---:|
| below threshold → start decision | 1000 ms | immediate |
| first generated frame after start | 7 ms first, 2 ms restart | 7 ms first, 2 ms restart |
| source change → first generated frame | ~1.1 s | ~0.1–0.2 s (EMA detection) |
| above threshold → stop decision | 1000 ms | 1000 ms safety |
| source change → generation stops | ~1.17 s | ~1.17 s |
| 0.5 s excursions | cancelled both directions | dips start at once; spikes ignored by the safety stop |

In a 60 fps capped pipeline (2×, target 60), the first generated frame arrived
**6.8 ms after the first present**, with presented fps at 120.2 and no safety
stop. That is the clean-pipeline case: the app cap supplies the pacing, so the
layer does not need to wait for a threshold dwell. In capped mode a very short
startup interval can be ignored for the first 200 ms to avoid a spurious
over-refresh stop.

The capped mode is intended for a game-side FPS limit at the source cap. It
removes the one-second startup wait and gives a steady generated cadence. The
adaptive mode remains the right choice when the application is uncapped and
the layer has to decide from the measured rate.

The cap is observable directly. On the same 165 Hz display:

| source | mode | cap | app fps | presented fps | present p50 |
|---:|---|---:|---:|---:|---:|
| 90 | 2x flow | auto (165) | 90.2 | 90.2 | **10.99 ms** |
| 90 | 2x flow | `OPENFG_REFRESH=200` | 90.2 | 165.2 | **33.01 ms** |
| 60 | 4x flow | auto (165) | 60.2 | 60.2 | **16.99 ms** |
| 30 | off | — | 30.2 | 30.2 | **33.00 ms** |
| 30 | 2x flow | auto (60) | 30.2 | 60.0 | **33.00 ms** |

The first row is the intended low-latency behavior: 90 × 2 = 180 presents/s
would not fit in 165 Hz, so the layer leaves the game native. The second row
shows what happens without the cap — the real frame lands two refresh
intervals later. The 60 fps 4x row wants 240 presents/s and is likewise left
native. `OPENFG_REFRESH` is also the fallback for sandboxes where the DRM card
nodes are hidden.

The 30 fps/60 Hz rows are the exact-boundary case, measured on the same
165 Hz panel with `OPENFG_REFRESH=60` rather than a physical 60 Hz mode. The
cap is `60 / 2 = 30 fps`, so the layer starts generating when the source is
at the cap (0.5% measurement tolerance) instead of requiring the manual
`target_fps`'s 5% headroom. 2x doubles the presented rate from 30 to 60 fps
without adding real-frame present latency: the p50 stays at 33 ms. This is
the main reason the presented cap is preferable to a hand-computed
`target_fps`.

The detected refresh is the active CRTC mode of the display card. If the
swapchain extent matches a mode, that connector wins; otherwise the
highest-refresh active connector is used. On a mixed-refresh multi-monitor
setup, a windowed game can therefore be capped for the faster display; set an
explicit `max_presented_fps` in that case.

With a concurrent GPU load, 4x at 24 fps also slipped to a p50 of ~83 ms.
Measure at the actual game rate and load.

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
| flow | 1.52 | 32.83 | 26.49 | 5×5 refinement |
| flow performance | 0.79 | 32.50 | 26.45 | half-res flow + 3×3 refine |
| blend | 0.02 | 20.81 | 22.53 | linear A/B blend |
| hold | 0.02 | 18.86 | 20.57 | repeat the current frame |

Quality flow is ~0.33 dB better on the pan scene for about twice the cost of
performance mode; ghosting is essentially unchanged (26.485 → 26.449 dB).
Performance mode is now the efficiency choice, while quality mode remains the
default. Blend and hold are comparison baselines, not shipping modes.

## Reproducing

```toml
[openfg]
enabled = true
generator = "flow"          # flow, blend, hold or off
pacing = "adaptive"         # adaptive or capped
max_presented_fps = -1      # detected display refresh; 0 = off
present_timing = true
debug = true
```

The layer logs the detected mode once, for example:

```text
[openfg] display: DP-1 2560x1440 @ 165.08 Hz
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

Some applications create a device where the present-id features are not
advertised (vkmark did here). The layer logs `present timing: unsupported` and
still reports app/presented fps, but not the present latency percentiles.

## What this does not measure

- Input-to-photon latency. It needs an external marker/camera or a hardware
  latency analyzer.
- GPU execution time separately from CPU submit time in the layer. `fg-lab`
  gives the clean GPU number; the layer's `overhead` includes present/copy
  submission.
