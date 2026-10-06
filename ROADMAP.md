# Roadmap — 26 weeks (2026-10-12 → 2027-04-09), 23 core + 3 optional
Each week = 4 work steps + 1 buffer session (done when `scripts/check WNN` passes all of that week's steps).
Every step fits one session. Its "Done when" is also saved as `checks/WNN.S.sh` (run with `scripts/check WNN.S`).
Each session ends with: update `docs/PROGRESS.md`, add anything new to `docs/LEARN.md`, `git push`.
`B=build/release`. Gates G1–G5 and experiment E1 are recorded in `docs/DECISIONS.md` as `## G1`, `## E1`, and so on.
**[core]** = needed for the goal. **[optional]** = cut it if you are behind. Skipping it never blocks a later core week.

## Phase A — Ground truth
### W01 [core] Toolchain, project files, machine baseline
- W01.1 Repo skeleton: CMake presets (release/debug/asan/ubsan/tsan), C++20, `dedup --version`, .clang-format, LICENSE, .gitignore;
  **CLAUDE.md** (build/test commands, main rules, including: "When explaining to the user, use simple English, short sentences,
  and a concrete example with real numbers."); **.claude/settings.json** with `permissions.deny: ["Read(./data/**)"]`;
  **docs/PROGRESS.md**; **docs/LEARN.md**; public GitHub repo.
  Done when: `cmake --preset release && cmake --build --preset release && $B/dedup --version && grep -q 'simple English' CLAUDE.md && python3 -c "import json;assert 'Read(./data/**)' in json.load(open('.claude/settings.json'))['permissions']['deny']" && test -f docs/PROGRESS.md -a -f docs/LEARN.md && gh repo view ChunChun451/dedup --json visibility -q .visibility | grep -qx PUBLIC`
- W01.2 Pinned FetchContent deps (BLAKE3, zstd, GoogleTest, google/benchmark). Smoke test. Done when: `ctest --preset release -R smoke`
- W01.3 System setup (you run both once): (a) write `C:\Users\chunc\.wslconfig` per D20, then `wsl --shutdown` from Windows;
  (b) `sudo scripts/setup-system.sh` (fio, hyperfine, libssl-dev, sudoers drop-caches rule).
  Done when: `free -g | awk '/^Mem:/{exit !($2>=11)}' && scripts/check-env.sh`
- W01.4 `bench/hw/run.sh`: fio seq read/write (32 GiB, O_DIRECT, 1 MiB, QD32), memory bandwidth 1/8/16 threads, host-cache test.
  Done when: `python3 bench/hw/validate.py results/hw/baseline.json`
- Buffer + **G1** (recalibrate numbers once if impossible). Done when: `grep -q '^## G1' docs/DECISIONS.md && scripts/check W01`

### W02 [core] Datasets & harness
- W02.1 `dedup-gen` (random/text/mixed/mutate, seeded, streaming). Done when: `ctest --preset release -R gen_golden`
- W02.2 Dataset scripts D1–D4 + *-small, sha256 manifests, free-space guard. Done when: `scripts/datasets/verify.sh small`
- W02.3 `bench/e2e/run.py` (TOML, cold/warm, Latin square, calibration, sync in timing, time -v, /proc io, JSON,
  disk-state rules from SPEC §6.12 via `bench/hw/diskstate.py`: session probe, per-second Windows disk counters,
  fast/slow drive-state label per cell, vhdx growth check).
  Done when: `python3 bench/e2e/run.py --tools cp --datasets d3-small --reps 3 --out results/smoke && python3 bench/e2e/validate.py results/smoke`
- W02.4 `io_ceiling` tool. Done when: `$B/io_ceiling --read data/d4-small --write-bytes 1G --json | python3 -m json.tool`

### W03 [core] Competitors & chunker foundation
- W03.1 Pinned restic/kopia/borg binaries into `tools/bin`, sha256 in `bench/tools.lock`. Done when: `scripts/tools/verify.sh`
- W03.2 Competitor baselines on d1-small, d3-small (warm, 3 reps) + restore check. Done when: `python3 bench/e2e/validate.py results/baseline-w03 --expect-tools restic,borg,kopia --expect-verified`
- W03.3 Chunker API in `lib/simdcdc` (streaming, span-based) + `dedup chunk-stats`. Done when: `ctest --preset release -R chunker_api`
- W03.4 FastCDC reference (Gear, normalization level 2); matches fastcdc-rs 2020 vectors (SekienAkashita.jpg). Done when: `ctest --preset release -R fastcdc_vectors`

## Phase B — SIMD chunking
### W04 [core] SeqCDC scalar
- W04.1 SeqCDC scalar (increasing mode, 5/50/512). Done when: `ctest --preset release -R seqcdc_scalar`
- W04.2 Cross-check against DedupBench (pinned commit, built in scratch). Done when: `tests/xval/seqcdc_dedupbench.sh`
- W04.3 `dedup analyze` (dedup ratio without storing). Done when: `$B/dedup analyze --chunker seqcdc data/d1-small --json | python3 tests/assert_keys.py saved_bytes unique_chunks`
- W04.4 **G2**: SeqCDC vs FastCDC dedup on D1–D3 (P8) → `docs/reports/g2-chunkers.md`. Done when: `grep -q '^## G2' docs/DECISIONS.md`

### W05 [core] SeqCDC AVX2 + profiler
- W05.1 Build `perf` from WSL2 kernel `tools/perf` (or record a fallback). Done when: `tools/bin/perf stat -e task-clock true`
- W05.2 AVX2 cut search (xor 0x80 unsigned trick, cmpgt, AND, movemask, tzcnt). Done when: `ctest --preset release -R seqcdc_diff_avx2`
- W05.3 AVX2 skip logic (popcnt + pdep n-th bit). Done when: `ctest --preset release -R seqcdc_diff_avx2_skip`
- W05.4 Streaming: random split sizes give the same cuts as one-shot. Done when: `ctest --preset release -R chunk_streaming`

### W06 [core] SeqCDC AVX-512 + dispatch
- W06.1 AVX-512BW (`_mm512_cmpgt_epu8_mask`). Done when: `ctest --preset release -R seqcdc_diff_avx512`
- W06.2 Runtime dispatch + `DEDUP_ISA` override. Done when: `for i in scalar avx2 avx512; do DEDUP_ISA=$i $B/dedup chunk-stats --digest-only data/d3-small/f0; done | sort -u | wc -l | grep -qx 1`
- W06.3 Tuning to P1a (L2-resident 1 MiB buffer) and report P1b (1 GiB from RAM, ≥ 0.85 × M1). Done when: `$B/bench_micro --benchmark_filter='SeqCDC/avx512/16K/L2' --benchmark_format=json | python3 bench/micro/assert.py --min-gbps 20`
- W06.4 libFuzzer differential fuzz, 15 min. Done when: `scripts/fuzz.sh seqcdc_diff 900`

### W07 [core] Experiment E1 + 1 MiB profile + chunker decision
- W07.1 E1: lane-parallel/unrolled Gear FastCDC (D19), AVX-512 + scalar-unrolled variants. Done when: `ctest --preset release -R fastcdc_fast_diff` (identical cuts to the reference on 10⁵ buffers + SekienAkashita vectors)
- W07.2 E1 decision: microbench vs reference, same params. Keep if ≥ 1.5× (P11). Otherwise record why and delete the code.
  Done when: `grep -Eq '^## E1 — (KEPT|DROPPED)' docs/DECISIONS.md && ( grep -q '^## E1 — DROPPED' docs/DECISIONS.md || $B/bench_micro --benchmark_filter='FastCDC' --benchmark_format=json | python3 bench/micro/assert.py --min-speedup 1.5 --vs 'FastCDC/ref' --of 'FastCDC/fast' )`
- W07.3 1 MiB profile: tune SeqCDC (SeqLength/SkipTrigger/SkipSize) and FastCDC for 512 KiB / 1 MiB / 8 MiB. Done when: `$B/dedup chunk-stats --chunk-avg 1M --json data/d3-small | python3 tests/assert_range.py mean_bytes 891289 1205862` (±15%)
- W07.4 **G3**: default chunker for 16 KiB and 1 MiB (speed + dedup). Done when: `grep -q '^## G3' docs/DECISIONS.md`

### W08 [core] Parallel chunking of one stream
- W08.1 Speculative segments + stitch (D4). Done when: `ctest --preset release -R parallel_chunk_equiv`
- W08.2 Adversarial inputs (zeros, cuts only at max size, periodic data) + fallback path. Done when: `ctest --preset release -R parallel_chunk_adversarial`
- W08.3 Multi-core chunk bench. Done when: `$B/bench_micro --benchmark_filter='ParChunk' --benchmark_format=json | python3 bench/micro/assert.py --min-speedup 5 --vs 'ParChunk/1' --of 'ParChunk/16'`
- W08.4 Phase B sanitizer sweep. Done when: `ctest --preset asan -R 'chunk|seqcdc|fastcdc' && ctest --preset ubsan -R 'chunk|seqcdc|fastcdc'`

## Phase L — Library release
### W09 [core] simdcdc v0.1.0 (C API, Rust, Go)
- W09.1 C API `lib/simdcdc/include/simdcdc.h` (opaque ctx, params, find_cuts, streaming feed, ISA query, version); static + shared lib;
  only `simdcdc_*` exported; C11 test. Done when: `ctest --preset release -R simdcdc_c_api && test -z "$(nm -D --defined-only $B/lib/simdcdc/libsimdcdc.so | awk '{print $3}' | grep -v '^simdcdc_')"`
- W09.2 Rust binding `bindings/rust` (install rustup per-user, pinned toolchain in `rust-toolchain.toml`; hand-written FFI + safe wrapper; cut digest test vs C).
  Done when: `cargo test --release --manifest-path bindings/rust/Cargo.toml`
- W09.3 Go binding `bindings/go` (Go tarball into `~/.local/go`, pinned in `bench/tools.lock`; cgo; digest test + benchmark).
  Done when: `(cd bindings/go && go test ./... && go test -bench . -run ^$ | python3 ../../bench/micro/assert_go.py --min-ratio-vs-c 0.95)`
- W09.4 Library README (API, example in C/Rust/Go, numbers table), standalone `simdcdc-bench`, tag **v0.1.0** + GitHub release.
  Done when: `$B/simdcdc-bench --quick && python3 scripts/doc_sections.py lib/simdcdc/README.md Install API Examples Benchmarks && git describe --tags --exact-match v0.1.0 && gh release view v0.1.0`

### W10 [optional] VectorCDC: RAM / VRAM → simdcdc v0.2.0
- W10.1 RAM scalar + VRAM AVX2 (tree-based max, packed scan). Done when: `ctest --preset release -R ram_diff_avx2`
- W10.2 VRAM AVX-512. Done when: `ctest --preset release -R ram_diff_avx512`
- W10.3 Microbench + dedup report (all chunkers × ISAs). Done when: `python3 bench/micro/report.py > docs/reports/chunkers.md && grep -q VRAM docs/reports/chunkers.md`
- W10.4 Revisit G3 (switch only if VRAM wins on speed **and** dedup); tag v0.2.0. Done when: `grep -q '^## G3b' docs/DECISIONS.md && git describe --tags --exact-match v0.2.0`

## Phase C — Hash, compress, encrypt, store
### W11 [core] Hashing & compression
- W11.1 BLAKE3 wrapper (plain + keyed), official vectors, ≥ 3 GB/s/core at 16 KiB. Done when: `ctest --preset release -R blake3_vectors && $B/bench_micro --benchmark_filter='Blake3/16K' --benchmark_format=json | python3 bench/micro/assert.py --min-gbps 3`
- W11.2 Fused chunk+hash on 1 MiB blocks (P2) + ID identity across ISAs/threads. Done when: `ctest --preset release -R chunk_id_identity && $B/bench_micro --benchmark_filter='ChunkHash/16' --benchmark_format=json | python3 bench/micro/assert.py --min-gbps 15`
- W11.3 zstd wrapper, per-thread contexts, raw-store rule. Done when: `ctest --preset release -R zstd_roundtrip`
- W11.4 Level sweep (−5…3) on D2/D3 samples + **G4**. Done when: `test -s docs/reports/zstd-levels.md && grep -q '^## G4' docs/DECISIONS.md`

### W12 [core] Encryption & pack format
- W12.1 Per-pack key derivation + AES-256-GCM with counter nonces (D8); tamper detection; ≥ 4 GB/s/core. Done when: `ctest --preset release -R crypto && $B/bench_micro --benchmark_filter='Gcm/16K' --benchmark_format=json | python3 bench/micro/assert.py --min-gbps 4`
- W12.2 `docs/FORMAT.md` v1. Done when: `python3 scripts/doc_sections.py docs/FORMAT.md Repository Pack Blob Encryption Index Tree Snapshot Versioning`
- W12.3 PackWriter/Reader (64 MiB, temp + rename + dir fsync), trailer, `dedup debug pack`. Done when: `ctest --preset release -R 'pack_roundtrip|pack_trailer'`
- W12.4 Crash test: 200 random SIGKILLs during pack writes. Done when: `ctest --preset release -R crash_pack`

### W13 [core] Index
- W13.1 Sharded open-addressing table, ≥ 50 M lookups/s at 16 threads. Done when: `ctest --preset release -R index_table && $B/bench_micro --benchmark_filter='IndexLookup/16' --benchmark_format=json | python3 bench/micro/assert.py --min-ops 5e7`
- W13.2 Index file persistence + merge. Done when: `ctest --preset release -R index_persist`
- W13.3 `rebuild-index` from pack trailers. Done when: `ctest --preset release -R rebuild_index`
- W13.4 20 M entries ≤ 1.2 GiB RSS. Done when: `scripts/mem_index.sh 20000000 1.2G`

### W14 [core] First end-to-end (single-threaded)
- W14.1 Tree + snapshot format. Done when: `ctest --preset release -R tree_format`
- W14.2 `init` + `backup`. Done when: `scripts/e2e/backup.sh data/d3-small`
- W14.3 `restore` + `verify_tree.py` (C1). Done when: `scripts/e2e/backup_restore.sh data/d1-small`
- W14.4 `check [--read-data]`, `snapshots`, corruption trials (C4). Done when: `ctest --preset release -R corruption_detect`

### W15 [core] Hardening
- W15.1 Encrypt all metadata; plaintext-leak test. Done when: `scripts/e2e/no_plaintext.sh`
- W15.2 Repo lock, error paths (EACCES, vanished/special files). Done when: `ctest --preset release -R error_paths`
- W15.3 20 GiB single file (> RAM) backup + restore, both chunk profiles. Done when: `scripts/e2e/backup_restore.sh data/d4/big0 && scripts/e2e/backup_restore.sh data/d4/big0 --chunk-avg 1M`
- W15.4 Full ASan/UBSan pass. Done when: `ctest --preset asan && ctest --preset ubsan`

## Phase D — Parallel pipeline at disk speed
### W16 [core] Pipeline core
- W16.1 `docs/PIPELINE.md` + bounded MPMC queue + aligned buffer pool with memory budget. Done when: `ctest --preset tsan -R 'queue|bufpool'`
- W16.2 Stages walker → readers → chunk+hash → lookup → compress+encrypt → pack writer → tree builder; `--null-store`. Done when: `scripts/e2e/backup_restore.sh data/d3-small --threads 16`
- W16.3 Determinism (C3). Done when: `scripts/e2e/determinism.sh data/d1-small`
- W16.4 `--stats-json` per stage. Done when: `$B/dedup backup --null-store --stats-json data/d3-small | python3 tests/assert_keys.py stages.chunk.busy_s stages.write.busy_s`

### W17 [core] Scaling to 8 cores
- W17.1 Scaling 1/2/4/8/16 threads. Done when: `python3 bench/e2e/scaling.py data/d3-small --assert-speedup 8:6`
- W17.2 Fix the top bottleneck (stats/perf), recorded. Done when: `grep -q 'W17.2' docs/DECISIONS.md`
- W17.3 Fix the next bottleneck, recorded. Done when: `grep -q 'W17.3' docs/DECISIONS.md`
- W17.4 P3 criteria. Done when: `python3 bench/e2e/check_criteria.py results/w17 --only P3a,P3b,P3c`

### W18 [core] Disk path
- W18.1 Reader: O_DIRECT vs buffered+fadvise, read-size sweep. Done when: `test -s docs/reports/read-path.md`
- W18.2 Writer: aligned writes, write-behind, batched fdatasync. Done when: `test -s docs/reports/write-path.md`
- W18.3 **G5**: io_uring needed? (yes only if reads < 0.9 × R). Done when: `grep -q '^## G5' docs/DECISIONS.md`
- W18.4 P4 cold-cache criteria (as far as possible without io_uring). Done when: `python3 bench/e2e/check_criteria.py results/w18 --only P4a,P4b --report`

### W19 [optional — do only if G5 says yes] io_uring
- W19.1 liburing via FetchContent (pinned) + feature probe on WSL kernel. Done when: `ctest --preset release -R uring_probe`
- W19.2 io_uring reader (registered buffers, QD tuning). Done when: `ctest --preset release -R uring_reader`
- W19.3 io_uring pack writer. Done when: `ctest --preset release -R uring_writer`
- W19.4 P4 re-check. Done when: `python3 bench/e2e/check_criteria.py results/w19 --only P4a,P4b`

### W20 [core] Small files & incrementals
- W20.1 Parallel walk (statx) + small-file batching. Done when: `python3 bench/e2e/run.py --tools dedup --datasets d1-small --reps 3 --out results/w20 && python3 bench/e2e/compare.py results/w18 results/w20 --metric files_per_s --min-ratio 1.5`
- W20.2 Files cache. Done when: `scripts/e2e/unchanged_reads_zero.sh data/d1-small`
- W20.3 Parent snapshot selection + `--no-files-cache`. Done when: `ctest --preset release -R files_cache`
- W20.4 Version-by-version incrementals on D1/D3. Done when: `python3 bench/e2e/validate.py results/w20-incr --expect-verified`

### W21 [core] Restore & robustness
- W21.1 Parallel restore (pack-ordered reads, parallel decode, pwrite). Done when: `python3 bench/e2e/check_criteria.py results/w21 --only P5c --self-only`
- W21.2 Soak: 10 changing snapshots, check + restore each. Done when: `scripts/e2e/soak.sh 10`
- W21.3 Fuzz parsers (pack trailer, index, tree), 15 min each. Done when: `scripts/fuzz.sh parsers 900`
- W21.4 TSan full pipeline + 200-kill crash test on backup (C5). Done when: `ctest --preset tsan && ctest --preset release -R crash_backup`

## Phase E — Benchmark campaign & release
### W22 [core] Freeze methodology
- W22.1 Final tool configs: defaults, matched, **same-chunk-size 1 MiB** (SPEC §6.3c). Pinned versions. Done when: `python3 bench/e2e/run.py --all --dry-run | grep -c '^CMD' | grep -qx "$(python3 bench/e2e/matrix.py --count)" && python3 bench/e2e/run.py --all --dry-run | grep -q 'DYNAMIC-1M-BUZHASH'`
- W22.2 All full datasets present + verified; virtual disk grown once (SPEC §6.12a). Done when: `scripts/datasets/verify.sh all && scripts/vhdx-pregrow.sh --check`
- W22.3 Variance study: ours + kopia 10×, CoV ≤ 5%; calibrate the drive-state thresholds (SPEC §6.12c) with fio runs in known fast and slow states. Done when: `python3 bench/e2e/variance.py results/w22-var --max-cov 0.05`
- W22.4 Pilot matrix, 1 rep (overnight). Done when: `python3 bench/e2e/validate.py results/pilot --complete`

### W23 [core] Run 1
- W23.1 Full matrix, 5 reps, resumable (overnight ×2). Done when: `python3 bench/e2e/validate.py results/run1 --complete --expect-verified`
- W23.2 Report generator (markdown + SVG). Done when: `python3 bench/e2e/report.py results/run1 > docs/reports/run1.md && grep -q 'P9' docs/reports/run1.md`
- W23.3 Criteria table (failures allowed here). Done when: `python3 bench/e2e/check_criteria.py results/run1 --report > docs/reports/run1-criteria.md`
- W23.4 Gap plan, one entry per failing criterion. Done when: `grep -q '^## Run1 gaps' docs/DECISIONS.md`

### W24 [optional] Close gaps
- W24.1–W24.4 Each session fixes one failing criterion from W23.4, worst first. Done when (per step): `python3 bench/e2e/check_criteria.py results/w24-<n> --only <criterion>`

### W25 [core] Run 2 + reproducibility
- W25.1 Full matrix run 2 (overnight ×2). Done when: `python3 bench/e2e/validate.py results/run2 --complete --expect-verified`
- W25.2 Fresh-clone reproduction (quick matrix). Done when: `scripts/reproduce.sh --quick`
- W25.3 All criteria. Done when: `python3 bench/e2e/check_criteria.py results/run2` (exit 0, or each exception documented in DECISIONS)
- W25.4 Final report. Done when: `python3 bench/e2e/report.py results/run2 > docs/reports/final.md`

### W26 [core] Release
- W26.1 README + usage docs. Done when: `python3 scripts/doc_sections.py README.md Install Quickstart Commands Benchmarks Limitations`
- W26.2 `scripts/ci.sh` (all presets, all tests, Rust/Go tests, 60 s fuzz each). Done when: `scripts/ci.sh`
- W26.3 Tag **v1.0.0** + GitHub release. Done when: `git describe --tags --exact-match v1.0.0 && gh release view v1.0.0`
- W26.4 Retrospective + future work. Done when: `test -s docs/RETROSPECTIVE.md`

## If you fall behind
Cut in this order: W24 (close gaps) → W10 (VRAM) → W19 (io_uring, if G5 said yes, accept the P4 shortfall and document it).
Core-only schedule = 23 weeks, so cutting all three still finishes by 2027-04-09.
