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
- Next: W01 buffer session = gate **G1**. Several SPEC targets are above these ceilings (see the session summary).

## Notes for later steps
- **W11.1:** measure BLAKE3 at exactly 16 KiB inputs (benchmark name `Blake3/16K`, matching the roadmap filter; today it is
  called `Blake3/16384`), and report which SIMD path is used (`blake3_simd_degree()`: 16 = AVX-512, 8 = AVX2, 4 = SSE4.1)
  in the benchmark output and in the W11.1 report.
