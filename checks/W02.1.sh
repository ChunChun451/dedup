#!/usr/bin/env bash
# W02.1 — dedup-gen: golden hashes, thread independence, edit rate; same bytes from GCC and Clang builds.
set -euo pipefail
cmake --build --preset release
ctest --preset release -R gen_golden
scripts/gen-crosscheck.sh
