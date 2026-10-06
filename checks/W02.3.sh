#!/usr/bin/env bash
# W02.3 — benchmark harness runs a 3-rep smoke matrix with the dummy cp tool and its results validate.
set -euo pipefail
cmake --build --preset release --target calib dedup-gen
rm -rf results/smoke
python3 bench/e2e/run.py --tools cp --datasets d3-small --reps 3 --out results/smoke
python3 bench/e2e/validate.py results/smoke --complete --expect-verified
