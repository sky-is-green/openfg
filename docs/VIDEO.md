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

Use a config that includes the player:

```toml
[openfg]
enabled = true
gpu = "auto"
cross_gpu = false
multiplier = 4
target_fps = 0
performance_mode = false

[processes]
include = ["mpv"]
exclude = []
```

On the reference system a 512x384, 23.976 fps H.264 file played with Vulkan
video decode and generated frames at both 2x and 4x. The real present rate
stayed at the content rate (~23.6 fps); the layer inserted the requested
frames. `4x` is the useful video setting: 24 fps becomes roughly 96 presented
frames/s before display pacing.

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

## Video-specific problems still open

| Problem | Current behaviour | Needed |
|---|---|---|
| Scene cuts | A cut between two unrelated frames can be blended. | Cut detection; skip the generated frame or output the current frame on a cut. |
| Cadence / timestamps | Layer always uses `t=0.5` between presented frames. | PTS-aware interpolation, especially 24 → 60/120/165 Hz. |
| Subtitles / OSD | Burned-in text is part of the frame. Static text is protected by the tile mask; moving text can ghost. | Subtitle-aware masking or a video-safe confidence threshold. |
| HDR / 10-bit | Layer path is 8-bit RGBA. | 10-bit/HDR swapchain and image path. |
| Film grain / noise | Optical flow can chase grain. | Optional prefilter or higher flow confidence threshold. |
| 24 fps to high refresh | 4x gives ~96 fps, then the display holds frames. | Dynamic frame count/timestamps from the player or compositor. |

`performance_mode = true` reduces interpolation cost, but video is usually
low-fps and not GPU-bound; `false` is the better starting point for quality.

## Recommended current setup

- `multiplier = 4`
- `target_fps = 0` (always generate; 24 fps is below any useful target)
- `performance_mode = false`
- Include the player in `processes.include`
- Start with mpv’s default/audio sync. `--video-sync=display-resample` can
  duplicate frames before the layer sees them, which is not what the
  interpolator wants.

A dedicated `[video]` profile (per-player multiplier/target and the cut guard)
is the next feature; the config currently has one global profile plus process
matching.
