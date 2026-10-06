# dedup — Specification v1.1 (2026-10-06, revised once at gate G1; frozen from here)

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
| P1a | SeqCDC, 1 core, D3 data in a 1 MiB buffer that stays in L2 cache (pure chunking speed) | AVX-512 ≥ 20 GB/s, AVX2 ≥ 12 GB/s |
| P1b | SeqCDC AVX-512, 1 core, 1 GiB D3 buffer read from RAM (reported together with P1a) | ≥ 0.85 × `M1` |
| P2 | Fused chunk+hash, 16 threads, in memory | ≥ 15 GB/s |
| P3a | Full pipeline, warm, null store, all-duplicate (files cache off) | ≥ 0.7 × warm re-read speed (`warm_read_8` median, measured in the same session) |
| P3b | Same, unique incompressible (D4-small) | ≥ 5 GB/s |
| P3c | Same, unique compressible (D3-small, ~2:1 with zstd level 1, measured at W02.1) | ≥ 2.5 GB/s |
| P4a | Cold, real repo on the same disk, first backup D2/D4 | time ≤ 1.15 × `io_ceiling` time |
| P4b | Cold, unchanged re-backup with files cache off | time ≤ 1.10 × read-only ceiling time |
| P5a | Warm first backup vs fastest competitor (defaults), every dataset | ≥ min(3 × fastest competitor, 0.87 × warm ceiling throughput) |
| P5b | Cold first backup | ≥ min(2 × fastest competitor, 0.87 × ceiling throughput) |
| P5c | Cold restore | ≥ min(2 × fastest competitor, 0.85 × `W`) |
| P5d | Unchanged incremental, files cache on | wall time ≤ fastest competitor |
| P6 | Repo size, matched compression, D1/D2/D3 | ≤ smallest competitor. D4 ≤ 1.01 × input |
| P7 | Peak RSS during backup | ≤ 1.5 GiB + 64 B × unique chunks |
| P8 | SeqCDC saved bytes vs FastCDC at 16 KiB on D1–D3 | within 3% relative, otherwise G2 switches the default |
| P9 | **Same chunk size (1 MiB avg), warm first backup, vs fastest competitor in the same-size config** | ≥ min(3 × fastest competitor, 0.87 × warm ceiling throughput) (pure code speed) |
| P10 | simdcdc via Rust and Go bindings, 1 GiB buffer, SeqCDC AVX-512 | ≥ 95% of the C++ throughput |
| P11 | E1 fast FastCDC vs reference FastCDC, same params, 1 core | ≥ 1.5× to keep it (otherwise dropped, not a failure) |

Definitions used above:
- *ceiling throughput* (P4a, P5b) = input bytes ÷ `io_ceiling` time for reading the input and writing our repo's bytes
  on the same disk. `io_ceiling` measures two modes, *sequential* (read everything, then write) and *overlapped* (read and
  write at the same time); the ceiling uses the faster (shorter) of the two.
- *warm ceiling throughput* (P5a, P9), per dataset and per tool: `T_warm` = max(input bytes ÷ warm re-read median,
  repo bytes that tool wrote ÷ `W`); warm ceiling = input bytes ÷ `T_warm`. It is a max, not a sum, because warm reads
  come from RAM and a pipeline reads the next piece while the previous one is being written (G1 correction).
  P5a and P9 use our own repo bytes. `W` and the warm re-read median come from the hardware baseline
  (`bench/hw/run.sh`) of the same benchmark session.
  Example, D4-small today: max(4.295 GB ÷ 13.59 GB/s, 4.32 GB ÷ 1.812 GB/s) = max(0.316 s, 2.385 s) = 2.385 s
  → 1.80 GB/s; the P5a target is then min(3 × fastest competitor, 0.87 × 1.80 = 1.57 GB/s).

Known limits: highly compressible unique data is bound by zstd (P3c), not the disk. Backups of new incompressible
data are bound by the disk's write speed `W` (~1.8 GB/s), for every tool.

## 5. Datasets (budget 170 GB; `scripts/datasets/verify.sh` fails if C: has < 30 GB free)
| ID | Content | Size | Why |
|---|---|---|---|
| D1 | Linux kernel trees v6.0…v6.12 (13 trees, cdn.kernel.org tarballs) | ~17 GB, ~1M files | Small files, many versions |
| D2 | 10 Debian 13 `genericcloud` release builds 2026-01-12 … 2026-05-01 (`disk.raw`, 3 GiB each, sparse; pinned SHA-512) | 30 GiB logical | Large files, VM-image dedup |
| D3 | `dedup-gen` mixed (50% text-like, 25% structured, 25% random; zstd-1 ratio 1.96:1), base + 2 versions at 0.5% edits | 3 × 16 GiB | Controlled shift resistance and compressibility |
| D4 | `dedup-gen` random (ChaCha20, seed 4) | 32 GiB | Incompressible, I/O-bound, defeats caches |
| *-small | D1 one tree; D3/D4 at 4 GiB | ≤ 4 GiB | Warm-cache runs |
Only scripts and digests are in git: `scripts/datasets/sources.txt` (pinned downloads), `scripts/datasets/manifest.txt`
(digest of every dataset file or tree). `scripts/datasets/make.sh small|all` builds, `verify.sh small|all` checks.
At most one competitor repo exists at a time.

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
6. **Cold cache**: `sync`, then drop the guest page cache (`sudo -n /usr/local/sbin/dedup-drop-caches`). No flush file:
   W01.4/G1 showed Windows does not cache the virtual disk (host-cache ratio 0.86–0.92). **Warm**: one unmeasured run,
   then measured runs, *-small datasets only.
7. **Timing**: wall time from process start to exit **plus a following `sync`**. `/usr/bin/time -v` gives CPU and peak RSS. `/proc/<pid>/io` gives bytes; `du -sb` gives repo size.
8. **Repetitions**: 5 per cell. Tool order rotates in a Latin square. A 2 s calibration benchmark runs before each cell, and the cell is re-run after a
   60 s cooldown if it deviates >5%. Report median, min/max, and CoV. A cell with CoV > 5% is flagged.
9. **Each tool's restore is verified** (sha256 manifest) at least once per dataset. A tool that fails gets no speed number.
10. **Raw JSON** for every run (git SHA, versions, `uname`, WSL memory). Reports and criteria come only from JSON.
11. **Caveat in every report**: WSL2 virtual-disk numbers understate raw NVMe. The comparison is fair because every tool runs on the same path.
12. **Disk state** (G1: write speed depends on recent history, not only on the tool). `bench/hw/diskstate.py` implements:
    (a) Before the campaign (W22), grow the virtual disk once by writing and deleting a 200 GB file, and never run
        `wsl --manage Ubuntu --compact` during the campaign. Reason: writes that make `ext4.vhdx` grow run at ~1.25 GB/s
        instead of ~1.8 GB/s, because Windows writes ~30% extra bytes.
    (b) Each session starts with the hardware baseline (`bench/hw/run.sh --initial-idle 1200`): 20 min without writes,
        then one 16 GiB write probe whose average and last 5 s must be ≥ 1.5 GB/s. This gives the session's `W`.
        There is no probe before each cell: for ~300 write-heavy cells it would add ≥ 5 TB of writes and up to 250 h
        of waiting (DECISIONS.md G1).
    (c) During every cell the harness logs Windows' per-second disk counters (`\PhysicalDisk(_Total)\Disk Write Bytes/sec`,
        `Avg. Disk Write Queue Length`, `Avg. Disk Bytes/Write`). A second is *drive-limited* when the write queue is
        ≥ 8 deep with writes ≥ 256 KiB (the drive, not the tool, sets the pace). A write-heavy cell (≥ 4 GiB written)
        with ≥ 5 drive-limited seconds is in the *fast* state if the median NVMe rate over those seconds is ≥ 1.4 GB/s,
        otherwise *slow*. These thresholds are provisional and calibrated in W22.3 with fio runs in known states.
    (d) Tool order inside every (dataset, workload, config) block is a Latin square, so tools share drive states evenly.
    (e) A comparison (P4a, P5a–c, P9) is valid only if the majority drive state over the 5 reps is the same on both sides.
        Otherwise those two tools' 5 reps are re-run after a 15 min rest, at most 5 such re-runs per night; the rest move
        to the next night. Results keep their state label in every report.
    (f) Record the `ext4.vhdx` size before and after every cell. If it grew by more than 1 GB, the cell is invalid and re-run.
    (g) Warm ceilings are the median of 10 runs, never a single run (single warm re-reads ranged 8.8–17.0 GB/s).
