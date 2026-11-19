# Companion scaler design

Status: design only. The layer remains responsible for frame generation and
the activation gate; the scaler is a separate capture/upscale/present app.

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

### Gamescope-style (default)

- The game runs inside a nested compositor at a lower internal resolution.
- The capture source is the compositor's composed output texture.
- Input, present, VRR and HDR stay inside the compositor.
- Integration options: fork/patch Gamescope and add an upscale pass, or build a
  small compositor around the same model. Verify the license and maintenance
  cost before forking.
- This is the recommended core because it solves the hard capture, input and
  display problems in one place.

### PipeWire (later drop-in)

- Capture a game window through xdg-desktop-portal/KDE screencast.
- Import DMA-BUF into Vulkan, upscale, present in a fullscreen borderless
  window.
- Keep the game focused and make the overlay click-through (empty input
  region).
- Risks: portal permission prompts, window selection, HDR metadata, VRR,
  compositor latency and input-focus edge cases.
- The upscaler and presenter must not change when this backend is added.

## Interfaces

```cpp
struct Frame {
    VkImage image;
    VkSemaphore ready;
    VkExtent2D extent;
    VkFormat format;
    VkColorSpaceKHR colorSpace;
    uint64_t sequence;
    uint64_t timestampNs;
    bool hdr;
};

class CaptureSource {
public:
    virtual ~CaptureSource() = default;
    virtual bool start() = 0;
    virtual bool next(Frame& frame) = 0;
    virtual void stop() = 0;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual bool prepare(const Frame& input, VkExtent2D output) = 0;
    virtual bool run(const Frame& input, VkImage output,
                     VkSemaphore done) = 0;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual bool present(const Frame& frame) = 0;
};
```

Keep the frame ownership and synchronization explicit. The PipeWire backend
will add a DMA-BUF import step behind `CaptureSource::next`.

## Upscalers

### Spatial baseline

- FSR1 EASU + RCAS-style sharpening, or an equivalent edge-adaptive spatial
  filter.
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

## Milestones

1. Gamescope capture + EASU/RCAS + fullscreen present at 1080p -> 1440p.
2. Integer scaling, aspect ratio, sharpness, present timing.
3. HDR and VRR.
4. Neural spatial backend behind the same interface.
5. PipeWire capture backend behind the same interface.
6. Upscale/FG ordering study.

## Licensing

MIT-compatible code only. FidelityFX FSR1/EASU/RCAS code is MIT; verify model
and dataset licenses before shipping weights. No NVIDIA DLSS code/weights and
no Lossless Scaling assets.
