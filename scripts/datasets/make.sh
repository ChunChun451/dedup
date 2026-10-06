#!/usr/bin/env bash
# Create datasets (SPEC.md section 5). Already-present files are kept; run verify.sh afterwards.
#   scripts/datasets/make.sh small   D1-small, D3-small, D4-small (~10 GB)
#   scripts/datasets/make.sh all     everything (~130 GB logical): D1, D2, D3, D4 and the small sets
source "$(dirname "$0")/common.sh"
what=${1:-}
[[ $what == small || $what == all ]] || die "usage: make.sh small|all"

make_small() {
  need_space 12
  extract_kernel 6.12 "$DATA/d1-small"                  # D1-small: one kernel tree
  gen mixed 3 4G "$DATA/d3-small/f0"                    # D3-small: 4 GiB mixed
  gen random 44 4G "$DATA/d4-small/f0"                  # D4-small: 4 GiB random
}

make_all() {
  need_space 130
  for v in 6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10 6.11 6.12; do
    extract_kernel "$v" "$DATA/d1"                      # D1: one directory per release
  done
  grep -E '^sha512 .*genericcloud-amd64-.*\.tar\.xz$' "$ROOT/scripts/datasets/sources.txt" | while read -r _ _ url; do
    build=$(basename "$(dirname "$url")")
    [[ -f $DATA/d2/$build/disk.raw ]] && continue
    tarball=$(fetch "$url")
    log "extracting $build"
    mkdir -p "$DATA/d2/$build.part"
    tar -xJf "$tarball" -C "$DATA/d2/$build.part" --sparse --no-same-owner
    mv "$DATA/d2/$build.part" "$DATA/d2/$build"         # D2: one directory per image build
  done
  gen mixed 3 16G "$DATA/d3/v0/f0"                      # D3: base + 2 versions, 0.5% edits each
  mutate 31 "$DATA/d3/v0/f0" "$DATA/d3/v1/f0"
  mutate 32 "$DATA/d3/v1/f0" "$DATA/d3/v2/f0"
  gen random 4 20G "$DATA/d4/big0"                      # D4: 32 GiB random in two files
  gen random 5 12G "$DATA/d4/big1"
  make_small
}

"make_$what"
log "done; now run: scripts/datasets/verify.sh $what"
