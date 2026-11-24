#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare Vulkan gate features against the fixed test vector.

Example:
  python3 tools/gate-compare.py --csv /tmp/gate.csv \
    --expected ../openfg-models/gate-v1/test-vector/expected.json \
    --gate ../openfg-models/gate-v1/gate-q0.json
"""

import argparse
import csv
import json
import math
import sys
from pathlib import Path

FEATURE_TOLERANCE = 0.002
SCORE_TOLERANCE = 0.01


def score(features, model):
    mean = model["scaler_mean"]
    scale = model["scaler_scale"]
    coef = model["coef"]
    logit = model["intercept"]
    for value, center, spread, weight in zip(
            features, mean, scale, coef):
        logit += ((value - center) / spread) * weight
    return 1.0 / (1.0 + math.exp(-logit))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--expected", type=Path, required=True)
    parser.add_argument("--gate", type=Path, required=True,
                        help="Portable gate JSON (q0 or q50).")
    args = parser.parse_args()

    expected = json.loads(args.expected.read_text(encoding="utf-8"))
    with args.csv.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if len(rows) != 1:
        print(f"expected one tile, got {len(rows)}", file=sys.stderr)
        return 1

    names = expected["feature_names"]
    values = [float(rows[0][name]) for name in names]
    reference = expected["features"]
    errors = [abs(a - b) for a, b in zip(values, reference)]
    print(f"features: {len(errors)}")
    print(f"  max abs error:  {max(errors):.8f}")
    print(f"  mean abs error: {sum(errors) / len(errors):.8f}")
    for name, value, ref, error in zip(names, values, reference, errors):
        if error > FEATURE_TOLERANCE:
            print(f"  {name}: {value:.8f} vs {ref:.8f} ({error:.8f})")

    model = json.loads(args.gate.read_text(encoding="utf-8"))
    actual = score(values, model)
    reference_score = expected["gate_scores"][model["name"]]
    delta = abs(actual - reference_score)
    print(f"gate {model['name']}: {actual:.8f} vs "
          f"{reference_score:.8f} (delta {delta:.8f})")

    ok = max(errors) <= FEATURE_TOLERANCE and delta <= SCORE_TOLERANCE
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
