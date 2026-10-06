#!/usr/bin/env bash
# Measure disk and memory ceilings (roadmap W01.4). Takes about 1-2 minutes; needs about 40 GB free.
# Laptop must be plugged in, with Windows power mode "Best performance", and no heavy apps running.
#   bench/hw/run.sh [extra options for bench/hw/run.py, see --help]
set -euo pipefail
cd "$(dirname "$0")/../.."
cmake --build --preset release --target membw >/dev/null
exec python3 bench/hw/run.py "$@"
