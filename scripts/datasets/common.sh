# Shared helpers for scripts/datasets/*.sh (sourced, not run).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DATA=$ROOT/data
ARCHIVES=$DATA/archives
GEN=$ROOT/build/release/tools/gen/dedup-gen
MIN_FREE_GB=30  # SPEC.md section 5: stop if C: has less than this free

log() { echo "[$(date +%T)] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# need_space <GB>: fail unless both C: (where the virtual disk lives) and the Linux disk keep MIN_FREE_GB after adding GB.
need_space() {
  local want=$1 c l
  c=$(df -B1G --output=avail /mnt/c | tail -n1 | tr -d ' ')
  l=$(df -B1G --output=avail "$ROOT" | tail -n1 | tr -d ' ')
  (( c - want >= MIN_FREE_GB )) || die "C: has ${c} GiB free; writing ${want} GiB more would leave less than ${MIN_FREE_GB}"
  (( l - want >= MIN_FREE_GB )) || die "Linux disk has ${l} GiB free; need ${want} + ${MIN_FREE_GB}"
}

# fetch <url>: download into $ARCHIVES once, checking the pinned hash from sources.txt. Prints the local path.
fetch() {
  local url=$1 line algo hash file
  line=$(grep -F " $url" "$ROOT/scripts/datasets/sources.txt") || die "$url is not pinned in sources.txt"
  algo=${line%% *}; hash=$(echo "$line" | cut -d' ' -f2)
  file=$ARCHIVES/$(basename "$url")
  mkdir -p "$ARCHIVES"
  if [[ ! -f $file ]] || ! echo "$hash  $file" | "${algo}sum" --check --status; then
    log "downloading $(basename "$url")" >&2
    curl -fsSL --retry 3 -o "$file.part" "$url"
    echo "$hash  $file.part" | "${algo}sum" --check --status || die "hash mismatch for $url"
    mv "$file.part" "$file"
  fi
  echo "$file"
}

# gen <mode> <seed> <size> <path>: generate a file with dedup-gen unless it already exists.
gen() {
  local mode=$1 seed=$2 size=$3 out=$4
  [[ -x $GEN ]] || die "build dedup-gen first: cmake --build --preset release"
  [[ -f $out ]] && return 0
  mkdir -p "$(dirname "$out")"
  log "generating $out ($mode, seed $seed, $size)"
  "$GEN" "$mode" --seed "$seed" --size "$size" -o "$out.part"
  mv "$out.part" "$out"
}

# mutate <seed> <in> <out>: 0.5% edits (SPEC.md D3) unless the output already exists.
mutate() {
  local seed=$1 in=$2 out=$3
  [[ -f $out ]] && return 0
  mkdir -p "$(dirname "$out")"
  log "mutating $in -> $out (seed $seed, 0.5%)"
  "$GEN" mutate --seed "$seed" --rate 0.005 --in "$in" -o "$out.part" 2>/dev/null
  mv "$out.part" "$out"
}

# extract_kernel <version> <dest-dir>: unpack linux-<version>.tar.xz into <dest-dir>/linux-<version>.
extract_kernel() {
  local v=$1 dest=$2 tarball
  [[ -d $dest/linux-$v ]] && return 0
  tarball=$(fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$v.tar.xz")
  mkdir -p "$dest"
  log "extracting linux-$v into $dest"
  rm -rf "$dest/.linux-$v.part" && mkdir -p "$dest/.linux-$v.part"
  tar -xJf "$tarball" -C "$dest/.linux-$v.part" --no-same-owner
  mv "$dest/.linux-$v.part/linux-$v" "$dest/linux-$v" && rmdir "$dest/.linux-$v.part"
}
