# scaler-lab dataset format

Local captures stay local. This repository only stores the schema, tools and
synthetic/permissive test data. Do not commit game frames or clips.

## Directory layout

Both flat directories and a manifest are supported. The manifest is preferred
because it carries metadata and explicit splits.

```text
dataset/
  high/
    game-a/
      scene-01/
        0001.png
  low/
    game-a/
      scene-01/
        0001.png
  masks/
    game-a/
      scene-01/
        0001.png
  dataset.json
```

- `high` is the ground truth at the display resolution.
- `low` is the same frame at the sampled render resolution.
- `masks` is optional and marks HUD/text regions.
- Use lossless PNG for SDR training data. Use EXR/PNG16 for HDR when the
  loader supports it; v0 of `scaler_lab.py` reads 8-bit RGB.

## Manifest

See `dataset.example.json`. Required per item:

| Field | Meaning |
|---|---|
| `name` | Stable unique id |
| `low` | Path to the low-resolution image, relative to the manifest |
| `high` | Path to the high-resolution image, relative to the manifest |

Optional:

| Field | Meaning |
|---|---|
| `game` | Game id, used for held-out-game splits |
| `scene` | Scene id |
| `split` | `train`, `val` or `test` |
| `fps` | Source capture rate |
| `hdr` | Boolean |
| `ui_mask` | Optional HUD mask path |

The loader currently uses `name`, `game`, `scene`, `split`, `hdr`, `low` and
`high`. `fps` and `ui_mask` are reserved for the temporal and HUD metrics.

## Capture protocol

- Capture at the highest stable frame rate the game can hold.
- Keep the camera path deterministic for A/B comparisons.
- For spatial pairs, render the same frame at low and high resolution, or
  downsample the high-resolution frame with a known kernel.
- For temporal/FG sequences, capture 120 or 240 fps and keep consecutive
  frames. Do not drop frames between the pair and the ground truth.
- Include fast pans, thin geometry, particles, transparency, HUD text, camera
  cuts, SDR and HDR.
- Avoid video compression for training. Compression is useful as a separate
  augmentation, not as the source.
- Record the game, scene, resolution, frame rate, HDR state and capture tool
  in the metadata.
- Split by **game**, not by frame. A model that only sees held-out frames from
  a training game is not being tested for generalization.

## Manifest example

```json
{
  "name": "local-scaling-v1",
  "scale": 2,
  "items": [
    {
      "name": "game-a/scene-01/0001",
      "game": "game-a",
      "scene": "scene-01",
      "split": "train",
      "low": "low/game-a/scene-01/0001.png",
      "high": "high/game-a/scene-01/0001.png",
      "fps": 60,
      "hdr": false
    }
  ]
}
```

## Running the harness

```sh
python3 tools/scaler-lab/scaler_lab.py \
  --manifest dataset/dataset.json --split test \
  --backend lanczos --out report.csv
```

Use `--split all` to run every item. Use `--limit N` for a quick smoke test.

## Licensing

Game captures are not redistributable under the project's MIT license. Keep
them local, describe the data in model cards, and train public weights on
synthetic or permissively licensed data.
