#!/usr/bin/env bash
# Build dedup-gen with Clang and check it writes exactly the same bytes as the GCC build (DECISIONS.md D22).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
clang++ -std=c++20 -O2 -pthread -Itools/gen -o "$tmp/gen-clang" tools/gen/main.cpp tools/gen/gen.cpp
gcc_gen=build/release/tools/gen/dedup-gen
for m in random text structured mixed; do
  a=$($gcc_gen "$m" --seed 11 --size 3M | sha256sum)
  b=$("$tmp/gen-clang" "$m" --seed 11 --size 3M | sha256sum)
  [[ $a == "$b" ]] || { echo "GCC and Clang differ for mode $m"; exit 1; }
done
$gcc_gen random --seed 3 --size 8M -o "$tmp/in"
a=$($gcc_gen mutate --seed 4 --rate 0.02 --in "$tmp/in" 2>/dev/null | sha256sum)
b=$("$tmp/gen-clang" mutate --seed 4 --rate 0.02 --in "$tmp/in" 2>/dev/null | sha256sum)
[[ $a == "$b" ]] || { echo "GCC and Clang differ for mutate"; exit 1; }
echo "dedup-gen: GCC and Clang outputs identical"
