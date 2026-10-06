#!/usr/bin/env bash
# W01.3 — .wslconfig applied (>= 11 GiB visible) + system setup done (scripts/setup-system.sh).
set -euo pipefail
free -g | awk '/^Mem:/{exit !($2>=11)}'
scripts/check-env.sh
