# Log reports

`tools/openfg-report.py` summarises the layer log without opening the raw
file. It parses `session:`, `perf:`, `path:`, `adaptive:` and startup lines.

## Usage

```sh
python3 tools/openfg-report.py raw/capture.log
python3 tools/openfg-report.py raw/*.log --detail
python3 tools/openfg-report.py raw/capture.log --json
python3 tools/openfg-report.py raw/*.log --out report.md
```

The default output is a Markdown comparison table. `--detail` adds a section
per log; `--json` is useful for scripts.

## What the columns mean

| Column | Meaning |
|---|---|
| duration | Last reported session duration |
| app | Mean render rate from the `perf:` intervals |
| presented | Mean presented rate from the `perf:` intervals |
| gen | Mean generated-frame percentage from the `perf:` intervals |
| overhead | Mean layer work per real frame |
| p50 | Median frame interval from the `perf:` intervals |
| present p50 | Real-frame present p50 from the `session:` line, or `n/a` |
| starts/stops | Gate transitions seen in the log |

The detail section also reports:

- backend init mean and restart init mean
- first-generated-frame delay
- source/generate/present path means
- present p95/p99 when present timing was enabled

## Caveats

- `perf:` intervals include menus and loading screens. A 30 fps capture with a
  60 fps menu can show a mean above 30. Use the detail/path numbers for the
  in-game segment.
- `present p50` is `n/a` unless the capture used `present_timing = true`.
- A `session:` line is emitted every 30 seconds; the table uses the last one for
  duration and present latency, while `perf:` means cover the whole log.
- Gate churn is visible as high `starts/stops` counts and elevated overhead in
  the detail section.

## Example

```text
| log | duration | app | presented | gen | overhead | p50 | present p50 | starts/stops |
|---|---|---|---|---|---|---|---|---|
| tina2k-ffx2x-30.log | 210.4 s | 43.9 | 65.8 | 36% | 0.59 ms | 33.3 ms | n/a | 5/3 |
| tina2k-churn-120.log | 180.1 s | 105.3 | 181.7 | 41% | 3.81 ms | 8.9 ms | n/a | 77/75 |
```

The churn capture shows the cost directly: 77 starts, 75 stops and a much
higher mean overhead than the steady captures.
