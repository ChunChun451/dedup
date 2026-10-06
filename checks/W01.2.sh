#!/usr/bin/env bash
# W01.2 — pinned FetchContent deps (BLAKE3, zstd, GoogleTest, google/benchmark) + smoke tests.
set -euo pipefail
B=${B:-build/release}
cmake --preset release
cmake --build --preset release
ctest --preset release -R smoke
