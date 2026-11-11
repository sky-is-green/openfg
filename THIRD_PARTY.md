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

No lsfg-vk, Lossless Scaling, or other CC-BY-NC-ND code is present in this
tree.
