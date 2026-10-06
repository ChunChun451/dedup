# dedup — Specification v1.0 (2026-10-06)

## 1. What we build
1. **simdcdc** — a standalone chunking library: SeqCDC (scalar/AVX2/AVX-512), FastCDC (reference + fast
   variant if E1 passes), optional RAM/VRAM; runtime ISA dispatch; C API; Rust and Go bindings; microbenchmark.
2. **dedup** — a C++20 backup engine (Linux x86-64) built on simdcdc; the library's demo and end-to-end benchmark:
   `init`, `backup`, `restore`, `snapshots`, `check [--read-data]`, `rebuild-index`, `analyze`, `chunk-stats`.
   Pipeline: parallel walk → parallel read → SIMD CDC fused with keyed BLAKE3 → index lookup → zstd + AES-256-GCM
   (per-pack keys) for unique chunks only → 64 MiB packs → encrypted trees/snapshots. Files cache for incrementals.
   Chunk profiles: `--chunk-avg 16K` (default) and `--chunk-avg 1M`.
3. Determinism: the same input and key give the same chunk IDs and snapshot ID for any ISA or thread count.
4. Tools: `dedup-gen` (deterministic data), `io_ceiling` (I/O baseline), micro and end-to-end benchmark harness.

## 2. Out of scope (v1)
Remote/cloud backends; prune/forget/GC; passwords/KDF (keyfile only); multiple concurrent writers; FUSE mount;
xattrs/ACLs/hardlinks/sparse preservation; Windows/macOS; tuned ARM/NEON; secret/keyed chunking (restic and borg
have it, which is a documented difference); codecs other than zstd; GUI; publishing the bindings to crates.io or pkg.go.dev.

## 3. Reference machine
Lenovo Legion, AMD Ryzen 7 8845HS (Zen 4, 8C/16T, AVX-512), 16 GiB DDR5-5600 (WSL2: 12 GiB after W01.3),
SK Hynix PC801 1 TB NVMe PCIe 4.0, Windows + WSL2 (kernel 6.18, Ubuntu 26.04), ext4 virtual disk.
"8 cores" = all 16 hardware threads. Ceilings measured in W01.4 → `results/hw/baseline.json`:
`R`/`W` = sequential read/write (fio, O_DIRECT, 1 MiB, QD32); `M1`/`M16` = memory read bandwidth.

## 4. Success criteria
Gate **G1** (end of W01) may revise the performance numbers **once**, only if the measured ceilings make them impossible,
with the reason in `docs/DECISIONS.md`. After that they are frozen. `bench/e2e/check_criteria.py <dir>` prints PASS/FAIL per row.

### 4.1 Correctness (all must pass)
| ID | Criterion |
|---|---|
| C1 | Restore is byte-identical (sha256 + mode + mtime_ns + symlink targets) for every snapshot of every dataset |
| C2 | Cuts are identical across scalar/AVX2/AVX-512, threads 1–16, and C/Rust/Go bindings: ≥10⁵ random + adversarial buffers, plus 15 min differential fuzzing with no mismatch |
| C3 | Snapshot root ID is identical for `--threads 1,2,8,16` |
| C4 | `check --read-data` detects 100/100 random single-byte corruptions in packs, and 100/100 in index/tree files |
| C5 | ASan/UBSan/TSan clean. 200 random SIGKILLs during backup leave a repo that passes `check` |

### 4.2 Performance (16 KiB average, compression + encryption on, unless stated otherwise)
| ID | Criterion | Target |
|---|---|---|
| P1 | SeqCDC AVX-512, 1 core, 1 GiB in-memory D3 buffer | ≥ 20 GB/s (AVX2 ≥ 12 GB/s) |
| P2 | Fused chunk+hash, 16 threads, in memory | ≥ 15 GB/s |
| P3a | Full pipeline, warm, null store, all-duplicate (files cache off) | ≥ 10 GB/s |
| P3b | Same, unique incompressible (D4-small) | ≥ 5 GB/s |
| P3c | Same, unique compressible (D3-small, ~3:1) | ≥ 2.5 GB/s |
| P4a | Cold, real repo on the same disk, first backup D2/D4 | time ≤ 1.15 × `io_ceiling` time |
| P4b | Cold, unchanged re-backup with files cache off | time ≤ 1.10 × read-only ceiling time |
| P5a | Warm first backup vs fastest competitor (defaults), every dataset | ≥ 3× its throughput |
| P5b | Cold first backup | ≥ min(2 × fastest competitor, 0.87 × ceiling throughput) |
| P5c | Cold restore | ≥ min(2 × fastest competitor, 0.85 × `W`) |
| P5d | Unchanged incremental, files cache on | wall time ≤ fastest competitor |
| P6 | Repo size, matched compression, D1/D2/D3 | ≤ smallest competitor. D4 ≤ 1.01 × input |
| P7 | Peak RSS during backup | ≤ 1.5 GiB + 64 B × unique chunks |
| P8 | SeqCDC saved bytes vs FastCDC at 16 KiB on D1–D3 | within 3% relative, otherwise G2 switches the default |
| P9 | **Same chunk size (1 MiB avg), warm first backup, vs fastest competitor in the same-size config** | ≥ 3× its throughput (pure code speed) |
| P10 | simdcdc via Rust and Go bindings, 1 GiB buffer, SeqCDC AVX-512 | ≥ 95% of the C++ throughput |
| P11 | E1 fast FastCDC vs reference FastCDC, same params, 1 core | ≥ 1.5× to keep it (otherwise dropped, not a failure) |

Known limit: highly compressible unique data is bound by zstd (P3c), not the disk.

## 5. Datasets (budget 170 GB; `scripts/datasets/verify.sh` fails if C: has < 30 GB free)
| ID | Content | Size | Why |
|---|---|---|---|
| D1 | Linux kernel trees v6.0…v6.12 (13 trees, cdn.kernel.org tarballs) | ~17 GB, ~1M files | Small files, many versions |
| D2 | 10 Debian 13 `genericcloud` raw daily images | ~20 GB | Large files, VM-image dedup |
| D3 | `dedup-gen` mixed (50% text-like, 25% structured, 25% random), base + 2 versions at 0.5% edits | 3 × 16 GiB | Controlled shift resistance and compressibility |
| D4 | `dedup-gen` random (ChaCha20, seed 4) | 32 GiB | Incompressible, I/O-bound, defeats caches |
| *-small | D1 one tree; D3/D4 at 4 GiB | ≤ 4 GiB | Warm-cache runs |
Only scripts and sha256 manifests are in git. At most one competitor repo exists at a time.

## 6. How we measure speed fairly
1. **Same conditions**: same machine and WSL instance, source and repo on the same ext4 filesystem, AC power, Windows
   "Best performance", Legion "Performance" mode, no other heavy apps. Long runs happen overnight.
2. **Pinned versions** in `bench/tools.lock` (sha256). Exact commands in `bench/tools/<tool>.toml`. `run.py --dry-run` prints them all.
3. **Three configurations per tool**:
   (a) *defaults* — each tool as shipped, encryption on. Ours: 16 KiB.
   (b) *matched compression* — restic `--compression auto`, borg `-C zstd,1`, kopia `zstd-fastest`, ours default level.
   (c) *same chunk size, 1 MiB average* — restic default (512 KiB/1 MiB/8 MiB); borg `--chunker-params buzhash,19,23,20,4095`;
       kopia `--object-splitter DYNAMIC-1M-BUZHASH`; ours `--chunk-avg 1M` (512 KiB/8 MiB). Matched compression as in (b).
       This isolates code speed from chunk-size effects (criterion P9).
   Borg 1.x is single-threaded by design. It is reported as is, with no adjustment.
4. **Encryption on for all**: restic (mandatory), borg `repokey-blake2`, kopia AES256-GCM, ours AES-256-GCM.
5. **Workloads**: first backup into a fresh repo (`init` not timed); unchanged incremental with files cache on; re-backup with files
   cache off (restic `--force`, borg `--files-cache=disabled`, kopia `--force-hash=100`, ours `--no-files-cache`);
   version-by-version incrementals (D1, D2, D3); full restore of the last snapshot.
6. **Cold cache**: `sync`, then drop the guest page cache (sudoers rule), then read a 20 GiB flush file with O_DIRECT to evict any Windows-host
   cache (whether the host caches is tested in W01.4). **Warm**: one unmeasured run, then measured runs, *-small datasets only.
7. **Timing**: wall time from process start to exit **plus a following `sync`**. `/usr/bin/time -v` gives CPU and peak RSS. `/proc/<pid>/io` gives bytes; `du -sb` gives repo size.
8. **Repetitions**: 5 per cell. Tool order rotates in a Latin square. A 2 s calibration benchmark runs before each cell, and the cell is re-run after a
   60 s cooldown if it deviates >5%. Report median, min/max, and CoV. A cell with CoV > 5% is flagged.
9. **Each tool's restore is verified** (sha256 manifest) at least once per dataset. A tool that fails gets no speed number.
10. **Raw JSON** for every run (git SHA, versions, `uname`, WSL memory). Reports and criteria come only from JSON.
11. **Caveat in every report**: WSL2 virtual-disk numbers understate raw NVMe. The comparison is fair because every tool runs on the same path.
