#!/usr/bin/env bash
# One-time system setup for the dedup project (roadmap W01.3, DECISIONS.md D21).
# Run once from your normal user:   sudo scripts/setup-system.sh
# Safe to run again: every step checks or overwrites, nothing is added twice.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Please run with sudo:  sudo $0" >&2
  exit 1
fi
USER_NAME=${SUDO_USER:-}
if [[ -z $USER_NAME || $USER_NAME == root ]]; then
  echo "Run this with sudo from your normal user account, not from a root shell." >&2
  exit 1
fi

echo "== 1/3 Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  fio hyperfine \
  libssl-dev pkg-config \
  flex bison libelf-dev libdw-dev libtraceevent-dev libcap-dev

echo "== 2/3 Installing the cache-drop helper"
HELPER=/usr/local/sbin/dedup-drop-caches
cat > "$HELPER.tmp" <<'EOF'
#!/bin/sh
# Write all pending data to disk, then empty Linux's file cache (used for cold-cache benchmarks).
set -e
sync
echo 3 > /proc/sys/vm/drop_caches
EOF
install -o root -g root -m 0755 "$HELPER.tmp" "$HELPER"
rm -f "$HELPER.tmp"

echo "== 3/3 Allowing $USER_NAME to run only that helper without a password"
RULE=/etc/sudoers.d/dedup-drop-caches
printf '%s ALL=(root) NOPASSWD: %s ""\n' "$USER_NAME" "$HELPER" > "$RULE.tmp"
visudo -cf "$RULE.tmp"
install -o root -g root -m 0440 "$RULE.tmp" "$RULE"
rm -f "$RULE.tmp"

echo
echo "Done. Now check everything (no sudo needed):  scripts/check-env.sh"
