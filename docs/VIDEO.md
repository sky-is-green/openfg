# Video

## What works today

The layer works with video players that present through Vulkan. The tested
case is **mpv 0.41** with its Vulkan output:

```sh
mpv --no-config \
    --vo=gpu-next \
    --gpu-api=vulkan \
    --gpu-context=waylandvk \
    --hwdec=auto-safe \
    /path/to/video.mkv
```

Give the player a dedicated profile. It overrides the game settings only for
the named processes, and `video.processes` is matched in addition to the normal
include list:

```toml
[openfg]
enabled = true
gpu = "auto"
cross_gpu = false
multiplier = 2
target_fps = 0
performance_mode = false

[processes]
include = ["helldivers2.exe"]

[video]
processes = ["mpv"]
multiplier = 4
target_fps = 0
performance_mode = false
safe = true
```

On the reference system a 512x384, 23.976 fps H.264 file played with Vulkan
video decode and generated frames at both 2x and 4x. `4x` is the useful video
setting: 24 fps becomes roughly 96 presented frames/s before display pacing.
`target_fps` is not useful for video and should stay at 0.

The GUI's **Video · 4x** preset fills in `processes = ["mpv"]` and enables the
cut guard. The player's process name is the native executable or the
Wine/Proton `.exe` name, not the window title.

## Scene-cut guard

A hard cut between two unrelated frames has no correct intermediate. Blending
across it produces a one-frame smear. With `safe = true` the layer computes a
single **cut score** per frame pair: the mean flow-compensated luma residual at
the coarsest pyramid level. Normal motion leaves a low residual; a cut leaves a
high one. Above the threshold the interpolate pass writes the current real
frame for every generated frame in that pair.

Measured on real video frames:

| pair | cut score | normal blend | safe |
|---|---:|---:|---:|
| hard cut (different scenes) | 0.184 | 12.21 dB | **99 dB** |
| consecutive frames, normal motion | 0.004 | 43.41 dB | 43.41 dB |

The comparison is against the current real frame, which is the correct output
for a cut. The threshold is 0.08. In a sample of live-action and animation content scored at 360p–480p, normal
pairs scored 0.004–0.024 and cuts scored 0.10–0.52; a fast pan scored 0.047 and
is deliberately left alone.

Cost: one workgroup reduces the level-4 residual, about **0.08 ms per frame
pair at 1440p** (1.55 → 1.63 ms in the harness). The pass only runs when the
video profile matches and `safe` is on.

Limitations:

- The score is global, so a cut between two visually very similar scenes can
  fall below the threshold. That is the conservative failure mode: it blends
  one pair rather than dropping frames unnecessarily.
- A full-screen flash, explosion or brightness jump can look like a cut. The
  fallback shows a real frame, so the visible effect is a short hold, not
  corruption.
- The guard does not fix cadence. See below.

## The important limitation

**This is a Vulkan layer, not a desktop compositor.** It only sees applications
that present through a Vulkan swapchain.

- mpv with `--vo=gpu-next --gpu-api=vulkan` works.
- VLC 3 on this system uses its OpenGL/VAAPI video output; it has no Vulkan
  libraries mapped, so the layer never sees it.
- Firefox and most desktop video players use GL/EGL or DMA-BUF/VAAPI paths,
  not a Vulkan swapchain, unless explicitly configured otherwise.

“Any video app” therefore needs one of three different integration points:

1. **Vulkan player** (works now): mpv/gpu-next, or any player that can use a
   Vulkan presentation path.
2. **Player filter/plugin** (best quality): integrate the interpolator with
   mpv/libplacebo or FFmpeg, where the player knows presentation timestamps and
   can request exact `t` values. This is the right home for film cadence.
3. **Compositor/display path** (any app): a gamescope/KWin-style output
   integration that generates frames after all clients have composited. This
   is much larger than a Vulkan layer and is not implemented.

## Remaining video problems

| Problem | Current behaviour | Needed |
|---|---|---|
| Scene cuts | Global cut score; the pair falls back to the current frame. | Done. Similar-scene cuts can still slip through. |
| Cadence / timestamps | Layer always uses `t=0.5` between presented frames (and 0.25/0.5/0.75 for 4x). | PTS-aware interpolation, especially 24 → 60/120/165 Hz. |
| Subtitles / OSD | Burned-in text is part of the frame. Static text is protected by the tile mask; moving text can ghost. | Subtitle-aware masking or a stronger confidence threshold. |
| HDR / 10-bit | Layer path is 8-bit RGBA. | 10-bit/HDR swapchain and image path. |
| Film grain / noise | Optical flow can chase grain. | Optional prefilter or a higher flow confidence threshold. |
| 24 fps to high refresh | 4x gives ~96 fps, then the display holds frames. | Dynamic frame count/timestamps from the player or compositor. |

`performance_mode = true` reduces interpolation cost, but video is usually
low-fps and not GPU-bound; `false` is the better starting point for quality.

## Recommended current setup

- `multiplier = 4`
- `target_fps = 0` (always generate; 24 fps is below any useful target)
- `performance_mode = false`
- `safe = true` (scene-cut guard)
- Name the player in `video.processes` (it is matched in addition to
  `processes.include`)
- Start with mpv’s default/audio sync. `--video-sync=display-resample` can
  duplicate frames before the layer sees them, which is not what the
  interpolator wants.
