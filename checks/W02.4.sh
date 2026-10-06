#!/usr/bin/env bash
# W02.4 — io_ceiling measures sequential and overlapped modes and reports the faster as the ceiling.
set -euo pipefail
B=${B:-build/release}
cmake --build --preset release --target io_ceiling
$B/io_ceiling --read data/d4-small --write-bytes 1G --json | python3 tests/assert_keys.py sequential.total_s overlapped.total_s ceiling_s ceiling_mode
