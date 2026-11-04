# FidelityFX optical-flow backend

`generator = "ffx"` replaces openfg's per-pixel flow estimator with AMD's
FidelityFX Optical Flow. The rest of the pipeline is unchanged: openfg's
photo-consistency interpolation still turns the flow field into the generated
frame.

## What is vendored

`src/openfg/shaders/ffx/` is FidelityFX Optical Flow 1.1.2 from the AMD
FidelityFX SDK v1.1.4 (commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`),
MIT-licensed. The original licence and the list of small driver-portability
edits are in `LICENSE.txt` and `NOTICE.md` in that directory. The host-side
pass sequencing, scene-change wiring, flow-to-pixel expansion and openfg
interpolation are this project's code.

FidelityFX is a block-based estimator: one motion vector per 8×8 block, with a
seven-level pyramid in quality mode and five levels at half resolution in
performance mode. openfg's own path uses a dense half-resolution field and a
full-resolution refinement pass.

## Quality and cost

1440p synthetic scenes, 100 `fg-lab` iterations, RX 7900 XT:

| backend | pan ms | pan dB | zoom dB | occlude dB | ghost dB | ghost excess |
|---|---:|---:|---:|---:|---:|---:|
| `flow` | 1.56 | 32.83 | 28.49 | 21.30 | 26.49 | 1.925 |
| `flow` performance | 0.85 | 32.50 | 28.34 | 21.24 | 26.45 | 1.951 |
| `ffx` | 0.87 | 33.27 | 31.92 | 21.19 | 26.60 | **1.904** |
| `ffx` performance | 0.42 | 33.07 | 31.76 | 21.17 | 26.56 | 1.938 |

FidelityFX is roughly twice as fast at the same setting and improves the
ghost-excess gate in both modes. The only synthetic regression is the thin
occluder scene (about 0.1 dB), where an 8×8 block contains both the occluder
and its background. Thin geometry is the known weak point of a block field.

## How it is wired

1. FidelityFX prepares an R8 luma image for each source layer.
2. Its luminance pyramid and scene-change detector run once per source pair.
3. The search, filter and scale passes produce `rg16i` block vectors at the
   flow resolution.
4. `ffx_flow_dense.comp` expands the block grid to a full-resolution RG16F
   field for openfg's interpolation pass. FidelityFX stores
   `previous = current + v`; openfg's interpolation expects the opposite sign,
   so the expansion negates it.
5. FidelityFX's scene-change detector keeps a short history across frames. The
   first comparison has no history, so its verdict is discarded and the first
   pair still gets flow. The history bit also feeds openfg's existing
   `videoSafe` fallback through `ffx_cut_score.comp`.

Performance mode first downsamples the colour frame to half resolution. There
are RGBA8 and RGBA16F variants, so 10-bit and HDR swapchains get the faster
path too.

## Try it

```sh
# Synthetic comparison
python3 tools/bench.py --gpu 0000:03:00.0 --iterations 100 --generator ffx
python3 tools/ghost_bench.py --scene ghost --generator ffx
python3 tools/ghost_bench.py --scene ghost --generator ffx --performance

# Real game
#   ~/.config/openfg/conf.toml
#   [openfg]
#   enabled = true
#   generator = "ffx"
```

The backend is selected per process; do not run it together with `afmf-linux`
or `lsfg-vk` on the same swapchain.

## Licensing

The project remains MIT. FidelityFX OF is AMD MIT code and is not clean-room
openfg code. If you redistribute a binary, keep the notices under
`share/licenses/openfg/ffx/` (installed by CMake) and `THIRD_PARTY.md`.
