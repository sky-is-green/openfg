# Showcase production plan

Goal: a short, honest YouTube showcase of OpenFG on Linux across several
games, with before/after motion, HUD close-ups, and frame-time evidence. The
video must show what frame generation actually does — and state what it does
not do.

## Claims and non-claims

**Show:**

- Native rendering versus OpenFG 2×/4× at identical game settings.
- Smoother motion when the native rate is below the display rate.
- HUD text staying sharp on generated frames (with a zoomed comparison).
- Frame-time and generation-cost evidence from the layer log.
- Multiple genres: open world, fast action, top-down ARPG, maybe RTS/racing.

**Do not claim:**

- Lower input latency. Generated frames are presented before the real frame,
  so real-frame latency increases.
- A higher application frame rate. The application renders at the same rate;
  only the presented rate rises.
- That the video shows the full benefit of 120/165 Hz output. YouTube playback
  is 60 fps, so the controlled comparison should use a 30 → 60 fps setup and
  say so.
- Any association with Lossless Scaling, AMD, or the game publishers beyond
  “running on.”

## Game matrix

Installed candidates on the reference machine:

| Game | Why include it | Suggested route | Capture mode |
|---|---|---|---|
| Monster Hunter Wilds | open world, quest HUD, already validated | camp walk with quest text, fast camera sweeps, a static HUD shot | 30 fps cap → 2× 60 |
| Helldivers 2 | fast combat, particles, camera motion | solo drop, patrol and combat, 60–90 s | 30/60 cap → 2× 60 |
| Path of Exile 2 | top-down ARPG, dense UI text, effects | town + a map section | 30/60 cap → 2× 60 |
| Rocket League | fast camera, deterministic training | free play / training pack | 30 → 2× 60, or 60 → 2× 120 slow-mo |
| Age of Empires II DE | RTS camera pan, HUD | recorded game replay | 30 → 2× 60 |
| Dune Awakening | open-world survival | skip initially (online state varies) | — |

Recommended first episode: **Wilds + Helldivers 2 + Path of Exile 2**.
Rocket League is a strong bonus for fast motion.

## Controlled A/B protocol

### 1. Fix the workload

- Resolution 2560×1440, fullscreen, same preset and same route.
- Disable dynamic resolution, upscalers and frame-gen features inside the
  game.
- Keep motion blur at the game default; note it. Motion blur can hide judder,
  so optionally record one extra clip with it off.
- Use the same time of day/weather where the game allows it.
- Record native and generated runs back-to-back; for online games use an
  offline/training mode if possible.

### 2. Make 30 → 60 visible at 60 fps video

YouTube playback is 60 fps. To show a real difference, cap the game at 30 fps
and cap OpenFG presentation at 60:

```toml
[openfg]
enabled = true
generator = "ffx"
multiplier = 2
performance_mode = true
pacing = "adaptive"
target_fps = 0
max_presented_fps = 60
```

- **Native run:** start the game with `DISABLE_OPENFG=1` (or `enabled = false`),
  game cap 30, record at 60 fps.
- **Generated run:** enable OpenFG, same game cap 30, record at 60 fps.
- The native file contains 30 unique frames/s; the generated file contains 60.
- Disclose the 30 fps cap on screen and in the description.

If a game has no 30 fps cap, use a frame limiter (MangoHud `fps_limit=30`
with the overlay hidden) and disclose it. Do not use the layer's
`target_fps` as a cap; it only starts/stops generation.

### 3. High-refresh segment (optional)

For a 165 Hz display, uncap the game and OpenFG, capture at 120 fps, and slow
the result to 50% for a short sequence. This shows individual generated
frames and HUD quality, not real-time feel. Label it clearly as slow motion.

### 4. Slate and alignment

- Put a 3-second slate (black card or a fixed in-game pose) at the start of
  both runs.
- Start the route at the same landmark; trim to the slate or a visible cue.
- Keep clips 45–90 s; use 15–25 s per game in the final edit.

## Capture

Use `tools/capture_window.py`. It records the X11/XWayland window or the full
display with ffmpeg `x11grab` at 60 fps:

```sh
# native run, whole 1440p display
python3 tools/capture_window.py --fullscreen --duration 60 \
    --output showcase/raw/wilds-native-30.mp4

# generated run
python3 tools/capture_window.py --fullscreen --duration 60 \
    --output showcase/raw/wilds-ffx2x-60.mp4
```

Windowed capture by title:

```sh
python3 tools/capture_window.py --title "Monster Hunter Wilds" \
    --duration 60 --output showcase/raw/wilds-ffx2x-60.mp4
```

Capture settings: 60 fps, H.264 CRF 14, `yuv420p`, faststart. No audio by
default (avoids Content ID on game music). If game audio is wanted, record it
separately and keep it low.

Before recording:

- Hide MangoHud/overlays for clean footage; optionally record one extra clip
  with the layer log/FPS overlay as evidence, or overlay a clean graph in post.
- Set the display to a fixed refresh mode (or use `max_presented_fps`) so the
  capture cadence is stable.
- Close other GPU-heavy applications. The reference card is the PowerColor
  RX 7900 XT at `0000:03:00.0`; keep the Sapphire card idle.

## Edit recipe

### Side-by-side motion

Use 1920×1080 per side and a 3840×1080 output, or 1280×720 per side for a
2560×720 output:

```sh
ffmpeg -y -i native.mp4 -i generated.mp4 -filter_complex "
[0:v]scale=1920:1080,drawtext=fontfile=/usr/share/fonts/liberation/LiberationSans-Bold.ttf:
 text='Native 30 fps':x=32:y=32:fontsize=44:fontcolor=white:
 box=1:boxcolor=black@0.55:boxborderw=12[a];
[1:v]scale=1920:1080,drawtext=fontfile=/usr/share/fonts/liberation/LiberationSans-Bold.ttf:
 text='OpenFG 2x - 60 presented fps':x=32:y=32:fontsize=44:fontcolor=white:
 box=1:boxcolor=black@0.55:boxborderw=12[b];
[a][b]hstack=inputs=2[v]" -map "[v]" -t 20 \
  -c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p \
  showcase/edits/wilds-compare.mp4
```

### HUD close-up

Crop the quest tracker (coordinates depend on the game and HUD scale) and
scale with nearest-neighbour to keep glyph edges visible:

```sh
ffmpeg -y -i generated.mp4 -vf \
  "crop=560:180:1960:230,scale=1680:540:flags=neighbor" \
  -t 10 -c:v libx264 -crf 14 -preset slow \
  showcase/edits/wilds-hud-generated.mp4
```

Do the same crop on the native clip and put them side by side. For the UI-mask
story, record one short clip with `OPENFG_NO_UI_MASK=1` and show the doubled
text next to the fixed version.

### Slow motion

For a 120 fps capture slowed to 50%:

```sh
ffmpeg -y -i wilds-120.mp4 -vf "setpts=2.0*PTS,fps=60" \
  -c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p \
  showcase/edits/wilds-slowmo.mp4
```

### Frame-time evidence

Extract the layer's `perf:` lines from `~/.local/state/openfg/layer.log`, plot
app fps, presented fps and overhead, and overlay the graph for 5–10 s. Label it
"measured by the layer, not a synthetic benchmark."

```sh
python3 tools/plot_layer_log.py --tail 240 --segment last     --title "Monster Hunter Wilds - 2x FFX"     --output showcase/edits/wilds-fps.png
```

The script splits sessions at gaps and plots one continuous run; pick a clean
route segment without loading screens.

### Thumbnail

Split frame: left “NATIVE 30”, right “OPENFG 2× 60”, large OpenFG wordmark,
dark background, no fake FPS counters. Use a real gameplay frame.

## YouTube package

**Title options:**

- `OpenFG: Open-Source Frame Generation on Linux — Before/After`
- `I Built an Open Frame-Generation Layer for Linux — 3 Games Tested`
- `Native vs OpenFG 2x: Monster Hunter Wilds, Helldivers 2, Path of Exile 2`

**Description template:**

```text
OpenFG is an MIT-licensed Vulkan frame-generation layer for Linux.
This is a before/after comparison, not a latency or “free FPS” claim.

Hardware: AMD Radeon RX 7900 XT, RADV/Mesa ..., CachyOS, kernel ...
Settings: 2560x1440, ... , native 30 fps cap vs OpenFG 2x at 60 presented fps.
Frame generation improves motion smoothness and increases the presented frame
rate; it adds display latency and does not increase the application's render
rate.

Chapters
00:00 What OpenFG is
00:45 How the layer works
01:30 Monster Hunter Wilds — native vs 2x
03:30 HUD close-up and ghosting
05:00 Helldivers 2
06:30 Path of Exile 2
08:00 4x video / 24 -> 96
09:00 Limitations
09:45 Build and links

Technical white paper: https://github.com/sky-is-green/openfg/blob/main/docs/WHITEPAPER.md
Source and config: https://github.com/sky-is-green/openfg

No Lossless Scaling assets or code are used. FidelityFX Optical Flow is
AMD MIT-licensed code; notices are in the repository.
```

**Settings:** category Gaming, language English, “not made for kids,”
Standard YouTube license, allow embedding. Upload unlisted first for review,
then public. Add chapters and a pinned comment with the exact config and
hardware.

**Tags:** Linux gaming, frame generation, Vulkan, FidelityFX, optical flow,
open source, Monster Hunter Wilds, Helldivers 2, Path of Exile 2, AMD RDNA3.

## Upload

Uploading needs the channel account. Two options:

1. **Manual:** we produce the final MP4, thumbnail and metadata; you upload in
   YouTube Studio.
2. **Browser:** you sign in to YouTube Studio in the in-app browser and
   explicitly authorize the upload; we drive the upload form without changing
   account settings.

We will not access the account without that explicit authorization.

## Copyright and disclosure

- Use your own gameplay; avoid cutscenes and licensed music.
- Mute game audio or keep it low; Content ID can claim game music.
- Disclose the 30 fps cap and that OpenFG was used.
- Do not present generated frames as native gameplay.
- Keep the MIT/FidelityFX notices in the repository and description.

## Production checklist

- [ ] Choose 3–4 games and routes.
- [ ] Confirm each game has a 30 fps cap or an external limiter.
- [ ] Record native and generated runs with a slate.
- [ ] Record one HUD close-up and one 4x/video clip.
- [ ] Extract frame-time logs.
- [ ] Edit side-by-side, slow-mo and HUD segments.
- [ ] Make the thumbnail and metadata.
- [ ] Upload unlisted, review on a phone and a desktop, then publish.
