# Third-party code

- `thirdparty/vulkan/` and `thirdparty/vk_video/`: Khronos Vulkan-Headers,
  Apache License 2.0. See `thirdparty/vulkan/LICENSE.md`. This is the Linux
  subset the build uses; unused Vulkan-Hpp utilities and the non-Linux
  platform headers are omitted.
- `thirdparty/vk`: our own Vulkan-Hpp configuration shim, MIT.
- `src/openfg/shaders/ffx/`: AMD FidelityFX Optical Flow 1.1.2, vendored from
  FidelityFX SDK v1.1.4 (MIT), following the MIT-licensed `afmf-linux`
  portability port. The original licence and notice are in
  `src/openfg/shaders/ffx/LICENSE.txt` and `NOTICE.md`; the notice lists the
  small driver-portability edits. The host-side pass sequencing, scene-change
  wiring and dense-flow expansion are openfg code; the optical-flow passes
  themselves are AMD's shaders.

- `src/openfg/shaders/fsr1/`: AMD FidelityFX Super Resolution 1.0 (EASU and
  RCAS), vendored from `GPUOpen-Effects/FidelityFX-FSR` (MIT). The original
  licence is in `src/openfg/shaders/fsr1/LICENSE.txt`; the compute wrappers
  `fsr1_easu.comp` and `fsr1_rcas.comp` and the `scaler-gpu` harness are
  openfg code.

The companion scaler uses system libraries, not vendored code:

- SDL3 (zlib licence) for window and Vulkan-surface creation.
- PipeWire (MIT) for capture streams.
- D-Bus (`libdbus-1`, AFL-2.1 or GPL-2.0+) for the desktop portal.
- ncnn (BSD-3-Clause) as an optional runtime for the neural 2x backend. It is
  not vendored; builds without it retain the EASU/RCAS path.

They are optional build dependencies of `openfg-scaler` and do not affect the
layer's dependency-free core.

No lsfg-vk, Lossless Scaling, or other CC-BY-NC-ND code is present in this
tree.
