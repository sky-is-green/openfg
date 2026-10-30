# GPU support

openfg is a Vulkan layer plus a Vulkan compute backend. The GPU support
question is therefore mostly a Vulkan feature question.

## Quick check

Build and run the probe on the target machine:

```sh
cmake --build build -j"$(nproc)"
./build/openfg-gpu-probe
./build/openfg-gpu-probe --gpu 0000:03:00.0
```

It prints the device properties, the extensions and features the backend needs,
the interpolator's storage formats, and then **actually creates a 64x64 backend
context** for both modes:

```text
runtime same  : yes
runtime cross : yes
```

`--gpu` accepts the same forms as `fg-lab` and the layer config: a PCI address,
a device name, or `vendor:device`.

## Requirements

Same-GPU mode needs:

- Vulkan 1.2 or newer, and a compute queue.
- Timeline semaphores.
- `VK_KHR_synchronization2` (core in Vulkan 1.3).
- `VK_KHR_external_memory_fd` and `VK_KHR_external_semaphore_fd` with
  `OPAQUE_FD` import/export.
- `VK_FORMAT_R8_UNORM` and `VK_FORMAT_R16G16_SFLOAT` storage images.
- A storage/sampled/transfer-capable `VK_FORMAT_R8G8B8A8_UNORM`.

Cross-GPU mode additionally needs:

- `VK_EXT_external_memory_dma_buf`.
- Image export with `DMA_BUF` handles.
- The same timeline-semaphore protocol across both devices.

The probe's format-query lines are advisory. Some RADV versions do not
advertise linear DMA-BUF images through
`vkGetPhysicalDeviceImageFormatProperties2` even though the exact
create/export path the backend uses works. The runtime line is authoritative.

## Status by GPU

| GPU | Same GPU | Cross GPU | Notes |
|---|---|---|---|
| AMD RDNA3 (RX 7000, RADV) | **tested** | **tested** | Reference system: two RX 7900 XT, Mesa 26.2.5, kernel 7.3. |
| AMD RDNA2 (RX 6000, RADV) | expected | expected | Meets the same Vulkan requirements; run the probe. |
| AMD RDNA1 / Vega (RADV) | likely | likely | Older RADV; verify storage formats and DMA-BUF with the probe. |
| AMD Polaris / GCN | possible | unlikely | Vulkan 1.1/1.2 support varies; timeline and external memory features are the usual blockers. |
| NVIDIA Turing / Ampere / Ada (proprietary) | expected | expected for two NVIDIA cards | Same-GPU OpaqueFd and DMA-BUF are supported by the Linux driver; cross-GPU is untested. |
| NVIDIA + AMD mixed | expected | **unsupported** | DMA-BUF images can be shared, but timeline semaphores exported as `OPAQUE_FD` are driver-private. The current sync protocol cannot cross vendors. |
| Intel Arc / Gen12+ (ANV) | expected | expected for two Intel cards | Verify DMA-BUF and storage formats with the probe. |
| Mixed Intel/AMD or Intel/NVIDIA | expected | **unsupported** | Same cross-vendor semaphore limitation. |

“Expected” means the API requirements are present in the vendor driver but we
have not run it here. “Unsupported” means the current design has a hard
technical blocker, not just an untested path.

## Why mixed-vendor cross-GPU does not work

Cross-GPU currently exports the backend's timeline semaphore as
`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT` and imports it into the
application's device. Opaque FD handles are meaningful to the driver that
created them; on Linux they are not a portable cross-vendor semaphore ABI.

`SYNC_FD` is the portable Linux fence handle, but it is a one-shot binary fence,
not a timeline semaphore. Supporting mixed vendors would need a different
synchronization design — for example a `sync_file`/DRM-syncobj protocol with
binary semaphores and an explicit per-frame handoff — rather than the current
timeline-based path.

Same-GPU mode uses OpaqueFd too, but both devices are created by the same
driver, so that path is fine.

## Memory and performance notes

- The probe creates tiny images. Cross-GPU needs free VRAM for the source and
  destination images (three destination layers for 4x). If the target GPU is
  already full, allocations may fall back to GTT/system memory and the
  backend becomes much slower. Check `mem_info_vram_used` on the target card.
- Cross-GPU adds two DMA-BUF copies and cross-device semaphore round-trips.
  On the dual-7900-XT reference system it was slower than same-GPU in
  Helldivers 2 (about 119 vs 134 measured fps), so it is not the default.
- The layer only sees Vulkan applications. See [VIDEO.md](VIDEO.md) for the
  video-player implications.

## What to send if a GPU fails

Please include:

```sh
./build/openfg-gpu-probe --gpu <your-gpu>
vulkaninfo --summary
```

and the layer's startup lines (`OPENFG_DEBUG=1`). The probe goes through the
same `openfg::Instance` / `openfg::Context` / `exportFds` path as the layer, so
a runtime failure there is the useful error to report.
