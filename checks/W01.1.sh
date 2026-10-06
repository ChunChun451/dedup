#!/usr/bin/env bash
# W01.1 — repo skeleton, project files, public GitHub repo.
set -euo pipefail
B=${B:-build/release}
cmake --preset release
cmake --build --preset release
$B/dedup --version
grep -q 'simple English' CLAUDE.md
python3 -c "import json;assert 'Read(./data/**)' in json.load(open('.claude/settings.json'))['permissions']['deny']"
test -f docs/PROGRESS.md -a -f docs/LEARN.md
gh repo view ChunChun451/dedup --json visibility -q .visibility | grep -qx PUBLIC
