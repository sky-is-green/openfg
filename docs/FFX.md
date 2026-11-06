# FidelityFX optical-flow backend

`generator = "ffx"` replaces openfg's per-pixel flow estimator with AMD's
FidelityFX Optical Flow. The rest of the pipeline is unchanged: openfg's
photo-consistency interpolation still turns the flow field into the generated
frame. It is the default generator. Devices that do not meet its subgroup or
storage-format requirements fall back to `flow` at backend creation and log
the reason.

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
| `ffx` | 1.03 | 33.23 | 31.94 | 21.19 | 26.60 | **1.915** |
| `ffx` performance | 0.52 | 33.05 | 31.79 | 21.17 | 26.56 | 1.945 |

FidelityFX is roughly twice as fast at the same setting and improves the
ghost-excess gate in both modes. The only synthetic regression is the thin
occluder scene (about 0.1 dB), where an 8×8 block contains both the occluder
and its background. Thin geometry is the known weak point of a block field.

The FFX timings include the static-UI mask described below; it costs about
0.1 ms at 1440p in performance mode. Without it the pan scene is 0.04 dB
higher; the mask deliberately trades that for removing doubled HUD text.

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

The luma pass currently uses FidelityFX's linear-SDR transfer function. That is
correct for SDR and 10-bit linear content; a true HDR10/PQ source would need the
layer to pass its transfer function and luminance range for ideal motion
estimation. The existing openfg flow path also derives luma from the raw
values, so this is not a regression, but it is a known limitation.

## Static-UI mask

Frame generation warps the whole image, including the HUD. The text is
screen-locked while the background under it moves, so warping drags copies of
the glyphs into the surrounding background: the doubled-HUD ghost. A per-pixel
static-detail test only covers the glyph core; the anti-aliased halo around it
still takes the warped blend, and on a synthetic oracle built from real game
text the doubled copy survived every threshold and dilation shortcut tried
before this pass.

`ui_mask.comp` runs at 1/8 resolution. It marks an 8×8 block when at least
three of a 4×4 sample grid contain high-contrast detail that is identical in
both source frames: luma difference below 0.015 and a local luma gradient above
0.15. `ui_mask_dilate.comp` then dilates the block mask by one texel. The
interpolation pass samples the final mask bilinearly at the output position
and at both warped source positions. Testing the source positions is what
covers fast pans: the ghost copy of a glyph sits wherever the warp read it
from, which can be further away than any fixed halo. Where the mask is set,
the pass uses the unwarped pair `mix(a0, b0, t)` instead of the flow-warped
samples. The text stays sharp and its ghost halo disappears; only the small
background band inside the masked halo is cross-faded instead of warped.

The mask costs about 0.1 ms at 1440p in performance mode (two small compute
passes). It is deliberately conservative — false positives freeze moving
background — and the synthetic scenes stay within noise of the numbers above.
Set `OPENFG_NO_UI_MASK=1` to disable it for A/B testing.

The mask is wired into the FFX backend only; the openfg flow path keeps the
existing static-detail test. Known limits:

- UI that moves or fades between the two source frames is not detected and
  still goes through the flow blend.
- Very fast pans can move a glyph further than the dilation halo; the glyph is
  covered but the outermost ghost fringe may remain.
- The background inside the masked halo is cross-faded rather than
  motion-compensated. Keeping the halo tight is what makes that trade
  invisible in practice.

`tools/hud_bench.py` is the regression test: a 48-pixel pan with static text
(ghost 24 pixels out, beyond the fixed halo) must lose at least half of its
halo edge energy with the mask enabled, without losing full-frame PSNR.

## Try it

```sh
# Synthetic comparison
python3 tools/bench.py --gpu 0000:03:00.0 --iterations 100 --generator ffx
python3 tools/ghost_bench.py --scene ghost --generator ffx
python3 tools/ghost_bench.py --scene ghost --generator ffx --performance
python3 tools/hud_bench.py --gpu 0000:03:00.0

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
