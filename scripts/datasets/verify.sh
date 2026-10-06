#!/usr/bin/env bash
# Check datasets against the committed digests in scripts/datasets/manifest.txt (and the free-space rule).
#   scripts/datasets/verify.sh small|all
#   scripts/datasets/verify.sh --record small|all   compute digests and write them into manifest.txt (first time only)
source "$(dirname "$0")/common.sh"
record=0
[[ ${1:-} == --record ]] && { record=1; shift; }
what=${1:-}
case $what in
  small) sets=(d1-small/linux-6.12 d3-small/f0 d4-small/f0) ;;
  all)   sets=(d1-small/linux-6.12 d3-small/f0 d4-small/f0)
         for v in 6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10 6.11 6.12; do sets+=("d1/linux-$v"); done
         while read -r b; do sets+=("d2/$b/disk.raw"); done < <(grep -oE 'trixie/[0-9]{8}-[0-9]+' "$ROOT/scripts/datasets/sources.txt" | cut -d/ -f2)
         sets+=(d3/v0/f0 d3/v1/f0 d3/v2/f0 d4/big0 d4/big1) ;;
  *) die "usage: verify.sh [--record] small|all" ;;
esac

MANIFEST=$ROOT/scripts/datasets/manifest.txt
touch "$MANIFEST"
need_space 0
fail=0
for s in "${sets[@]}"; do
  path=$DATA/$s
  if [[ ! -e $path ]]; then echo "MISSING  $s"; fail=1; continue; fi
  t0=$(date +%s.%N)
  got=$(python3 "$ROOT/scripts/datasets/treehash.py" "$path")
  secs=$(printf '%.1f' "$(echo "$(date +%s.%N) - $t0" | bc)")
  want=$(awk -v s="$s" '$2 == s {print $1}' "$MANIFEST")
  if [[ -z $want ]]; then
    if (( record )); then echo "$got $s" >> "$MANIFEST"; echo "RECORDED $s (${secs} s)"; else echo "UNPINNED $s"; fail=1; fi
  elif [[ $got == "$want" ]]; then
    echo "OK       $s (${secs} s)"
  else
    echo "MISMATCH $s (${secs} s)"; fail=1
  fi
done
exit $fail
