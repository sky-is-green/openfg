# PipeWire capture probe

`capture-test.py` is a feasibility probe for the future PipeWire capture
backend. It asks the desktop portal for a monitor stream and captures one frame
through GStreamer.

This is not the companion overlay. It validates the hard part of the PipeWire
path before the C++ capture source and Vulkan presenter are written.

## Requirements

- PyGObject with Gio
- GStreamer with `pipewiresrc`, `videoconvert` and `pngenc`
- A running xdg-desktop-portal ScreenCast implementation

## Usage

```sh
python3 tools/scaler-pipewire/capture-test.py \
  --output /tmp/openfg-pipewire.png
```

The first request opens a desktop screen-sharing permission dialog. Approve
the monitor you want to capture. `--node-only` prints the PipeWire node id
without capturing.

## What it proves

Tested on KDE Plasma with the ScreenCast portal and DP-1 at 2560x1440:

- the portal session/select/start sequence works
- the monitor is exposed with `source_type = 1` and the right size
- the in-process GStreamer pipeline consumes the node and writes one frame

The captured frame is deleted after the test. The portal session closes when
the probe exits.

It does not prove input passthrough, HDR, VRR or low-latency presentation.
Those belong to the overlay implementation in `docs/SCALER.md`.

## Next

1. Persist the portal restore token so permission is not requested every run.
2. Replace GStreamer with a DMA-BUF import into the Vulkan presenter.
3. Add a click-through fullscreen window and present timing.
