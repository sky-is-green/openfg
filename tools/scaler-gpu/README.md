# scaler-gpu

Standalone Vulkan EASU/RCAS baseline for the companion scaler work. It reads
raw RGBA8, upscales with FidelityFX Super Resolution 1.0, and writes raw RGBA8
at the output size.

This is a lab tool, not the final compositor. It exists so the app lane can
measure the spatial baseline before the neural model and the Gamescope pass
are wired in.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target scaler-gpu
```

## Usage

```sh
# Convert a PNG to raw RGBA (ffmpeg)
ffmpeg -y -i low.png -f rawvideo -pix_fmt rgba low.rgba

./build/scaler-gpu \
  --input low.rgba --output high.rgba \
  --width 1280 --height 720 \
  --out-width 2560 --out-height 1440 \
  --filter easu+rcas --iterations 10

# Convert the result back
ffmpeg -y -f rawvideo -pix_fmt rgba -s 2560x1440 -i high.rgba high.png
```

Options:

| Option | Meaning |
|---|---|
| `--filter easu` | EASU only |
| `--filter rcas` | RCAS only; no scaling |
| `--filter easu+rcas` | EASU then RCAS (default) |
| `--sharpness N` | RCAS sharpness, 0 (max) to 20 (min) |
| `--iterations N` | Timed passes after one warm-up |
| `--gpu auto|PCI|name` | Device selection |

The output is RGBA8. `--out-width/--out-height` default to 2x the input.

## Timing

The tool records Vulkan timestamps around each pass and prints `gpu avg`,
`gpu min` and a wall-clock average. The first pass is a warm-up and is excluded
from the GPU timing; the wall average still includes it.

Measured on the RX 7900 XT / RADV, 1280x720 -> 2560x1440, 10 iterations:

| Filter | GPU avg |
|---|---:|
| EASU | ~0.14 ms |
| RCAS | ~0.06 ms |
| EASU+RCAS | ~1.4 ms |

The combined path includes the intermediate storage image and the barrier
between the passes; measure it, don't infer it from the single-pass numbers.

## Quality

Use `tools/scaler-lab/scaler_lab.py` for PSNR/SSIM/LPIPS against a high-res
reference, or compare the output PNG against the reference with the same
metrics. On the synthetic `ghost` scene, EASU+RCAS was slightly below Lanczos
on PSNR/SSIM; this is a baseline, not a final quality verdict. Real-game
content and a reference renderer matter.

## Licence

FSR1 EASU/RCAS is AMD FidelityFX code under the MIT licence; see
`src/openfg/shaders/fsr1/LICENSE.txt`. The wrapper shaders and this harness are
openfg code.
