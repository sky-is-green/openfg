# Companion scaler design

Status: first slice implemented. The layer remains responsible for frame
generation and the activation gate; the scaler is a separate
capture/upscale/present app in the same repository.

Implemented in `openfg-scaler`:

- `CaptureSource` with a deterministic test pattern and a PipeWire ScreenCast
  source (monitor or window, DMA-BUF import with a shared-memory fallback).
  The source fixates the negotiated DRM modifier and announces a single
  buffer block; `--no-dmabuf` forces the shared-memory path.
- FSR1 EASU with optional RCAS, reusing the embedded shaders and constants.
- SDL3 Vulkan swapchain presenter with aspect-fit letterboxing, windowed or
  fullscreen.
- Optional `--present-timing` using `VK_KHR_present_id` and
  `VK_KHR_present_wait` for capture-to-display latency.
- `--crop WxH+X+Y` for window decorations and monitor sub-rectangles.
- Headless presenter with an RGBA8 frame dump for deterministic tests.
- Frame/CPU timing summary and optional progress output.

Not in this slice: click-through input passthrough, HDR/VRR, integer scaling,
restore-token persistence, explicit PipeWire sync, neural inference and the
Gamescope capture backend.

## Repository layout

The scaler is a **module and executable in this repository**, not a separate
project or repository. It shares `vkutil`, the configuration code, shader
embedding, CI, docs and releases with the layer.

```text
src/scaler/
  main.cpp
  scaler.hpp                     CaptureSource / Upscaler / Frame
  vulkan.hpp / vulkan.cpp        context and readback helpers
  capture/test_pattern.*         deterministic synthetic source
  capture/pipewire.*             portal + PipeWire DMA-BUF source
  upscale/easu.*                 FSR1 EASU + RCAS
  upscale/fsr1_constants.*       shared CPU-side constants
  present/presenter.hpp          presenter interface + factory
  present/pipeline.*             fullscreen present pass
  present/sdl_presenter.*        SDL3 swapchain presenter
  present/headless_presenter.*   offscreen presenter and dump
```

The CMake target is `openfg-scaler` and installs alongside `openfg-layer`,
`openfg-gui` and `openfg-tray`. PipeWire is an optional build dependency; the
core layer remains dependency-free. Gamescope remains an external compositor
that the scaler can launch or integrate with.

## Build and run

```sh
cmake -S . -B build -G Ninja
cmake --build build --target openfg-scaler

# Deterministic test pattern in a window.
./build/openfg-scaler --source test --windowed \
  --input 1280x720 --output 2560x1440

# Headless self-test with a raw RGBA8 dump.
./build/openfg-scaler --headless --source test --static \
  --input 640x360 --output 1280x720 --frames 1 \
  --dump /tmp/scale-out.rgba --dump-input /tmp/scale-in.rgba

# Capture the selected window through the desktop portal. Crop the KDE title
# bar so the source is exactly 1280x720.
./build/openfg-scaler --source pipewire --capture window \
  --input 1280x720 --crop 1280x720+0+28 \
  --output 2560x1440 --fullscreen --present-timing
```

The SDL3 presenter is built when `sdl3` is available through pkg-config.
PipeWire capture is built when `libpipewire-0.3` and `dbus-1` are available.
The test-pattern/headless path needs only Vulkan.

`--input` is the requested capture size and the test-pattern size. The portal
may negotiate a different size (a monitor stream is usually the monitor's
native size), so the upscaler adapts to the first frame's actual extent.
`--output` is the upscaled size; the presenter letterboxes it when the window
aspect differs. `--crop` applies after the portal negotiates the source size,
so use the reported size from the first run if the crop is rejected.
`--present-timing` waits for the display and reports capture-to-display
latency; leave it off for normal use. `--no-dmabuf` copies through shared
memory instead of importing a DMA-BUF; it is slower but useful for diagnosing
driver or modifier issues.

### Source resolution and modes

These are performance modes, not quality modes:

- **Performance:** 1080p -> 1440p (1.33x). Significantly sharper than 720p and
  playable, but still a spatial upscale.
- **Ultra performance:** 720p -> 1440p. A 4x pixel increase; visibly soft and
  noisy on high-detail content such as GravityMark's asteroid field.

Spatial upscaling cannot recover missing detail, so neither mode matches a
native 1440p render. A **quality** mode means native resolution or a
neural/higher-quality upscaler; the EASU path is not a quality mode and the
neural backend is not wired yet.

RCAS sharpness defaults to 4 (0 is maximum sharpening, 20 is minimum). Lower
values crisp edges but amplify noise and halos; higher values are cleaner and
softer. `--filter easu` disables RCAS entirely for the cleanest edges.

## Goal

Run a game at a lower internal resolution, upscale it to the display, and
present it fullscreen with predictable latency. Spatial upscaling comes first;
neural spatial upscaling and a capture-based temporal approximation are
research branches.

## Non-goals

- No injection into the game.
- No engine-data temporal upscaling until a game/engine partner provides
  motion vectors, depth and jitter.
- No game assets in the repository or public model releases.

## Pipeline

```text
CaptureSource -> Upscaler -> Presenter
```

A frame carries:

- image and extent
- format and color space
- HDR metadata, if any
- capture timestamp
- sequence number

The pipeline should be usable without the OpenFG layer. When the layer is
active, it runs at the game's internal resolution and the scaler upscales the
already-generated output.

## Capture backends

The capture backend is the only part that differs between the Gamescope and
PipeWire modes.

### Gamescope-style (recommended core, not implemented)

- The game runs inside a nested compositor at a lower internal resolution.
- The capture source is the compositor's composed output texture.
- Input, present, VRR and HDR stay inside the compositor.
- Integration options: fork/patch Gamescope and add an upscale pass, or build a
  small compositor around the same model. Verify the license and maintenance
  cost before forking.
- This is the recommended core because it solves the hard capture, input and
  display problems in one place.
- A prototype wrapper lives in
  [`tools/scaler-gamescope/`](../tools/scaler-gamescope/README.md). It uses
  Gamescope's built-in FSR/NIS/SGSR to validate the workflow before the
  custom pass is wired in.

### PipeWire (implemented drop-in)

- Capture a game window through xdg-desktop-portal/KDE screencast.
- Import DMA-BUF into Vulkan, upscale, present in a fullscreen borderless
  window.
- Keep the game focused and make the overlay click-through (empty input
  region).
- Risks: portal permission prompts, window selection, HDR metadata, VRR,
  compositor latency and input-focus edge cases.
- The upscaler and presenter must not change when this backend is added.
- `src/scaler/capture/pipewire.cpp` implements the production source:
  portal session, monitor/window selection, DMA-BUF import and a
  shared-memory fallback. It supersedes the GStreamer probe in
  `tools/scaler-pipewire/`.
- Still missing: click-through presentation, restore-token persistence and
  waiting on `SPA_DATA_SyncObj` for explicit synchronization.
- The scaler window is created hidden and shown only after capture starts, so
  it does not appear in the portal's window picker.

## Interfaces

```cpp
struct Frame {
    vk::Image image;
    vk::ImageView view;
    vk::Extent2D extent;
    vk::Format format;
    uint64_t sequence;
    uint64_t timestampNs;
};

class CaptureSource {
public:
    virtual bool start(std::string& error) = 0;
    virtual bool next(Frame& frame, std::string& error) = 0;
    virtual void stop() = 0;
    virtual std::string description() const = 0;
};

class Upscaler {
public:
    virtual void resize(vk::Extent2D input, vk::Extent2D output) = 0;
    virtual void record(vk::CommandBuffer command,
                        vk::ImageView input,
                        vk::Extent2D inputExtent) = 0;
    virtual vk::ImageView view() const = 0;
    virtual vk::Extent2D outputExtent() const = 0;
};

class Presenter { /* acquire, record, submit, pollEvents, saveFrame */ };
```

The capture source owns its image. The source leaves a frame in
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`; the upscaler writes an RGBA32F
image and leaves it in the same layout for the presenter. The presenter owns
the swapchain (or offscreen image), window, fences and semaphores.

The production PipeWire source uses a three-image ring. The main thread copies
an imported DMA-BUF into a ring image, waits for the copy fence, then returns
the buffer to PipeWire. This keeps buffer ownership and synchronization
explicit and avoids doing Vulkan work on the PipeWire data thread.

## Upscalers

### Spatial baseline

- FSR1 EASU + RCAS-style sharpening, or an equivalent edge-adaptive spatial
  filter. The standalone baseline is in
  [`tools/scaler-gpu/`](../tools/scaler-gpu/README.md).
- Integer scaling and aspect-ratio handling.
- 8-bit sRGB first; 10-bit/HDR after the format path is stable.

### Neural spatial

- Small residual CNN or U-Net, FP16.
- Target under ~3 ms at 1080p -> 1440p on the RX 7900 XT.
- Inference via IREE/SPIR-V or NCNN Vulkan; no mandatory system dependency.
- Model format versioned; a plugin API is a later step.

### Temporal approximation

- Recurrent model with estimated flow, warped history and a confidence gate.
- Reset on scene cuts; protect HUD/text; fall back to spatial on low
  confidence.
- Not a replacement for engine-data temporal upscaling.

## Presentation

- Fullscreen Vulkan swapchain.
- VRR and HDR where the compositor/display supports them.
- `VK_KHR_present_wait` for present latency where available.
- Internal timestamps for capture, upscale and present; report p50/p95/p99 and
  dropped/duplicated frames.
- A capture-to-present budget of roughly two frames is the first target.

## Frame generation order

Two options to measure:

1. `game -> layer FG -> capture -> upscale -> present` (upscale after FG).
2. `game -> capture -> upscale -> FG -> present` (FG at display resolution).

The first is cheaper and keeps the existing layer; the second can preserve more
detail but needs FG at display resolution. Do not choose by intuition; compare
quality, latency and GPU cost.

## Validation ladder

Use a deterministic Vulkan workload before a real game:

1. **`vkcube` smoke** — already installed. Run it under Gamescope at the
   target render size and confirm scaling, input and present behavior.
2. **GravityMark** — modern Vulkan benchmark with rasterization, mesh shader
   and ray-tracing modes, a CLI and regular updates. Run it at native 1440p
   and at 1280x720 -> 1440p through Gamescope FSR/NIS; compare frame time,
   GPU load and power. `vkmark` is older and only useful as a legacy
   comparison.
3. **Quality frames** — capture native and scaled output from the benchmark,
   then run the same frames through `scaler-gpu` EASU/RCAS and the neural
   model. Use `scaler-lab` for PSNR/SSIM/LPIPS and visual review.
4. **Real games** — final compatibility, HUD, particles, transparency and
   anti-cheat validation.

`vkcube` is a smoke test, not a performance benchmark. A benchmark scene also
does not replace game validation; it just removes the game from the early
plumbing loop. Basemark GPU is a second modern option but its free version
requires an internet connection and uploads results, so GravityMark is the
better local automation target.

GravityMark's free evaluation licence is for testing, not commercial
demonstration. Do not publish its footage without permission; use it for
internal validation and keep the public video on your own game captures or a
permissively licensed benchmark.

## Validation

- The test pattern at 640x360 -> 1280x720 with EASU+RCAS matches
  `scaler-gpu` within 1 LSB per channel on the same input frame.
- The headless test pattern runs at ~510 fps for that size on the RX 7900 XT;
  this measures the pipeline, not a game.
- The PipeWire path was validated on KDE Plasma/Wayland with a DP-1
  2560x1440 monitor stream: `BGRA`, linear DMA-BUF, one plane plus a sync
  object, imported into Vulkan and upscaled/presented. Captured test frames
  were deleted after checking the dimensions and pixel range.
- At 1280x720 -> 2560x1440, the GPU pass timings on the RX 7900 XT are
  ~0.32 ms p50 for EASU+RCAS and ~0.10 ms p50 for the present pass (test
  pattern, headless, timestamps around each pass).
- GravityMark 1.89 was used as the artificial game window: 1280x720 client
  area (1280x748 including the KDE title bar), cropped to 1280x720 and
  upscaled to 2560x1440 fullscreen. 240 frames took 3.96 s (60.58 fps);
  CPU time was 18.16 ms p50, and capture-to-display latency was 9.71 ms p50
  and 21.77 ms p95. The source ran at 60 Hz vsync, so the idle source interval
  is 16.7 ms. GravityMark is evaluation/testing-only; no footage was retained
  or published.
- Window capture uses the same portal/PipeWire code with `types = 2`; the
  window is selected in the portal dialog.

## Milestones

1. ~~PipeWire capture + EASU/RCAS + windowed/fullscreen present~~ (first
   slice; Gamescope capture is still a separate backend).
2. Integer scaling, aspect ratio, sharpness, present timing.
3. HDR and VRR.
4. Neural spatial backend behind the same interface.
5. ~~PipeWire capture backend behind the same interface.~~
6. Upscale/FG ordering study.

## Licensing

MIT-compatible code only. FidelityFX FSR1/EASU/RCAS code is MIT; verify model
and dataset licenses before shipping weights. No NVIDIA DLSS code/weights and
no Lossless Scaling assets.
