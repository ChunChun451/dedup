#!/usr/bin/env bash
# W01 buffer session — gate G1 decided and every W01 step still passes.
set -euo pipefail
grep -q '^## G1' docs/DECISIONS.md
scripts/check W01
