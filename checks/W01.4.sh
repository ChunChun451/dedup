#!/usr/bin/env bash
# W01.4 — hardware baseline measured and plausible (results/hw/baseline.json, made by bench/hw/run.sh).
set -euo pipefail
python3 bench/hw/validate.py results/hw/baseline.json
