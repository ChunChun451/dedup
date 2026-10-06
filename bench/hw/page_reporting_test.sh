#!/usr/bin/env bash
# Show the cost of WSL handing freed memory back to Windows ("free page reporting").
# Copies a 4 GiB file into a NEW file (needs freshly allocated memory) and overwrites it IN PLACE (reuses memory).
# On native Linux both take about the same kernel time. Prints the current page_reporting_order first.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=$HOME/.cache/dedup-page-reporting
mkdir -p "$D"
echo "page_reporting_order = $(cat /sys/module/page_reporting/parameters/page_reporting_order)"
build/release/tools/gen/dedup-gen mixed --seed 3 --size 4G -o "$D/src"
sync
for i in 1 2 3; do
  rm -f "$D/dst"; sleep 3   # freed memory gets reported to Windows after ~2 s
  /usr/bin/time -f "new file:   wall %e s, kernel %S s" cp "$D/src" "$D/dst"; sync
  /usr/bin/time -f "in place:   wall %e s, kernel %S s" dd if="$D/src" of="$D/dst" bs=1M conv=notrunc status=none; sync
done
rm -rf "$D"
