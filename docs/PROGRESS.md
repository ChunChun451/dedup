# Progress

One entry per session, newest first. Each entry says which roadmap steps were finished and how they were checked.

## 2026-10-06 — Session 1: planning + W01.1–W01.3
- Measured the machine, read the VectorCDC/SeqCDC papers and the restic/borg/kopia chunkers.
- Wrote SPEC.md, ROADMAP.md, docs/DECISIONS.md (D1–D20, D20b). Public repo: https://github.com/ChunChun451/dedup
- `.wslconfig` written; the user restarted WSL. Verified: 11 GiB RAM, 16 CPUs, 4 GiB swap.
- W01.1 done: `scripts/check W01.1` PASS (presets, `dedup --version` → `dedup 0.0.1`, CLAUDE.md, data/** deny rule).
- W01.2 done: `scripts/check W01.2` PASS (4/4 smoke tests). The same smoke tests also pass under asan, ubsan and tsan.
  First number: BLAKE3 hashes 3.46 GiB/s on one core at 16 KiB (W11.1 needs ≥ 3 GB/s).
- W01.3 scripts written (setup-system.sh, check-env.sh, D21). Before the sudo run, check-env passes everything except
  the 6 items the script installs. **Waiting for the user to run `sudo scripts/setup-system.sh`.**
- W01.3 done: the user ran `sudo scripts/setup-system.sh`; `scripts/check W01.3` PASS.
- BLAKE3 follow-up: the 3.46 GiB/s came from `Blake3/16384` (one 16 KiB input). Runtime probe: `blake3_simd_degree()` = 16,
  so the **AVX-512** path was active. Same 16 KiB through each path's leaf hashing: AVX-512 4.17, AVX2 2.90, SSE4.1 1.46 GiB/s.
  The public API gets 3.50 GiB/s; the rest of the time goes to the 15 parent-node merges of the BLAKE3 tree plus init/finalize.

- W01.4 done: `scripts/check W01.4` PASS. `bench/hw/run.sh` (fio + new `membw` tool), laptop on AC, "Best performance".
  | | run 1 (baseline.json) | run 2 (baseline-run2.json) |
  |---|---|---|
  | R: seq read, 32 GiB, O_DIRECT | 5.89 GB/s | 6.14 GB/s |
  | W: seq write, 32 GiB, O_DIRECT | 1.48 GB/s | 1.74 GB/s |
  | M1 / M16: RAM read, 1 / 16 threads | 21.6 / 23.1 GB/s | 21.0 / 23.4 GB/s |
  | warm page-cache read, 8 readers | 8.8 GB/s | 12.9 GB/s |
  | host caches the virtual disk? | no (ratio 0.92) | no (ratio 0.86) |
  Bugs found while building it: the compiler deleted membw's reads (fixed with a barrier); fio empties the page cache
  before each job unless `--invalidate=0` (fixed). Huge pages make no difference to RAM bandwidth (23.3 vs 23.0 GB/s).
  The WSL disk file `ext4.vhdx` grew from 2.9 GB to 40 GB; ext4 reuses that space for the datasets, so no compaction now.
- Gate G1 (W01 buffer), at the user's request first: target-vs-limit table, write-speed investigation (per-second logs,
  Windows NVMe counters, native Windows write), 10× warm re-reads, C: free-space recheck. Findings and the table are in
  DECISIONS.md G1; raw numbers in results/hw/g1-investigation.json. SPEC.md v1.1: P1 split into P1a/P1b (0.85 × M1),
  P3a relative (0.7 × warm re-read, not lowered), P5a/P9 capped by the measured write limit (D4-small: 1.38 GB/s),
  no flush file, disk-state rules (§6.12: session probe + Windows counters, no per-cell probe).
- `bench/hw/run.py` now rests the disk, probes with 16 GiB until fast, logs per-second write speed, records virtual-disk
  growth and takes the median of 10 warm reads. The first schema-2 baseline was rejected by the new validator
  (probe 8 GiB passed, then the 32 GiB write fell to 1.0 GB/s), which led to the stronger 16 GiB probe.

- G1b (user correction): warm ceiling = max(read, write) time, not the sum; io_ceiling gets two modes.
- W02.1 done: `dedup-gen` (D22). Found and fixed a C++ argument-evaluation-order bug: GCC and Clang produced different
  structured/mixed data; `scripts/gen-crosscheck.sh` now guards it. Measured zstd-1 ratios: mixed 1.96:1 (SPEC said ~3:1).
- W02.2 done: dataset scripts (D23); D2 uses Debian release builds because dailies are deleted. Small sets built.
- W02.3 done: harness `bench/e2e/run.py` (D24); `scripts/check W02.3` PASS (6/6 cells, restore verified).
- W02.4 done: `io_ceiling` (D25); preallocating the write file fixed a 2-3x understated write speed.
- **Open issue (blocks trustworthy numbers):** WSL "free page reporting" is on (kernel log: "Free page reporting
  enabled", order 5). Memory Linux frees is handed back to Windows; using it again costs ~1-2 s of kernel time per GiB.
  Measured: copying 4 GiB into new memory 6.4-10.2 s, overwriting in place 1.5 s. The cp smoke cells varied 7.5-25 s.
  WSL 3.0.1 has no setting to turn it off; root can raise `page_reporting_order` (max 10 = only 4 MiB blocks).
  Test: `bench/hw/page_reporting_test.sh`. Needs the user's sudo to try.

## Notes for later steps
- **W11.1:** measure BLAKE3 at exactly 16 KiB inputs (benchmark name `Blake3/16K`, matching the roadmap filter; today it is
  called `Blake3/16384`), and report which SIMD path is used (`blake3_simd_degree()`: 16 = AVX-512, 8 = AVX2, 4 = SSE4.1)
  in the benchmark output and in the W11.1 report.
