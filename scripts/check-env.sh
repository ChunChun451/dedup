#!/usr/bin/env bash
# Check that this machine is ready for the dedup project. Needs no sudo and changes nothing.
# Prints one PASS/FAIL line per item; exit code 0 only if all pass.
set -uo pipefail

fail=0
check() {  # check "<description>" <command...>
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then
    echo "PASS  $what"
  else
    echo "FAIL  $what"
    fail=1
  fi
}
version_ge() {  # version_ge <have> <need>
  [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

# Hardware and WSL settings (.wslconfig, D20)
check "RAM visible to Linux >= 11 GiB" awk '/^MemTotal:/{exit !($2 >= 11*1024*1024*0.95)}' /proc/meminfo
check "16 logical CPUs" test "$(nproc)" -eq 16
for flag in avx2 bmi2 avx512f avx512bw avx512vl avx512vbmi vaes; do
  check "CPU flag $flag" grep -qw "$flag" /proc/cpuinfo
done

# Toolchain
check "g++ >= 15" version_ge "$(g++ -dumpfullversion)" 15
check "clang++ >= 21 (for fuzzing)" version_ge "$(clang++ -dumpversion)" 21
check "cmake >= 3.28" version_ge "$(cmake --version | awk 'NR==1{print $3}')" 3.28
check "ninja" command -v ninja
check "python3 >= 3.11 (tomllib)" python3 -c 'import sys; assert sys.version_info >= (3, 11)'
check "gh logged in" gh auth status

# Installed by scripts/setup-system.sh
check "fio" command -v fio
check "hyperfine" command -v hyperfine
check "OpenSSL 3 headers (libssl-dev)" bash -c 'pkg-config --atleast-version=3.0 openssl'
check "perf build deps (flex, bison, libelf, libdw)" \
  bash -c 'command -v flex && command -v bison && test -f /usr/include/libelf.h && test -f /usr/include/elfutils/libdw.h'
check "cache-drop helper is root-owned" bash -c '[[ $(stat -c %U:%a /usr/local/sbin/dedup-drop-caches) == root:755 ]]'
check "cache-drop helper allowed without password" sudo -n -l /usr/local/sbin/dedup-drop-caches

# Disk space (SPEC.md section 5: stop if C: has less than 30 GB free)
check "C: has >= 30 GB free" bash -c '(( $(df -B1G --output=avail /mnt/c | tail -n1) >= 30 ))'
check "Linux disk has >= 30 GB free" bash -c '(( $(df -B1G --output=avail / | tail -n1) >= 30 ))'

exit $fail
