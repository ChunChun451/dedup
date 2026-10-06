# Decisions

Every technical decision and its reason. New decisions are appended. A changed decision gets a new entry that
points to the old one; old entries are never deleted. Gates (G1–G5) and experiments (E1) start as `## (pending) Gx` and are
renamed to `## Gx — <result>` when decided, because the roadmap checks look for `^## Gx`.

## Machine facts these decisions are based on (measured 2026-10-06)
- AMD Ryzen 7 8845HS (Zen 4), 8 cores / 16 threads, AVX-512 (F/BW/VL/DQ/VBMI/VBMI2/VPOPCNTDQ…), VAES, SHA-NI.
- L2 1 MiB per core, L3 16 MiB.
- 16 GiB DDR5-5600, one module (probably single-channel, ~45 GB/s theoretical). WSL2 sees 7.4 GiB before W01.3.
- SK Hynix PC801 1 TB NVMe PCIe 4.0, which Linux sees only through a WSL2 virtual disk (ext4). C: had 269 GiB
  (= 289 GB) free; `df -h` prints GiB, which this line first mislabeled as GB (corrected at G1).
- Windows + WSL2 (kernel 6.18), Ubuntu 26.04, GCC 15.2, Clang 21.1, CMake 4.2.3.

## Sources
- VectorCDC, FAST '25: https://www.usenix.org/conference/fast25/presentation/udayashankar
- VectorCDC extended (ACM TOS 2026): https://arxiv.org/abs/2508.05797
- SeqCDC, Middleware '24: https://cs.uwaterloo.ca/~alkiswan/papers/SeqCDC_Middleware24.pdf
- Vectorized SeqCDC (IEEE TPDS 2026): https://arxiv.org/abs/2505.21194
- Reference code: https://github.com/UWASL/dedup-bench
- restic chunker: https://github.com/restic/chunker (512 KiB min, 8 MiB max, 1 MiB average, 64 B window)
- borg internals: https://borgbackup.readthedocs.io/en/stable/internals/data-structures.html (buzhash 19/23/21/4095)
- kopia: https://kopia.io/docs/reference/command-line/common/repository-create-filesystem/ (DYNAMIC-4M-BUZHASH, BLAKE2B-256-128)

---

## D1 — Primary chunker: SeqCDC; FastCDC as reference; RAM/VRAM optional
**Decision:** SeqCDC with scalar, AVX2 and AVX-512 versions is the main chunker. FastCDC (scalar) is the trusted
reference, plus the fast variant from experiment E1. RAM/VRAM (VectorCDC) is optional (W10).
**Reason:** SeqCDC is the fastest published CDC at 8–16 KiB: 30.5 GB/s with AVX-512 at 16 KB in the TPDS paper,
1.23–1.35× faster than VRAM, with dedup within 4–6% of the best algorithm. FastCDC has public test vectors,
so it lets us check our test setup. E1 and VRAM are fallbacks if SeqCDC fails gate G2.

## D2 — Chunk sizes: 16 KiB default, 1 MiB profile
**Decision:** default average 16 KiB (SeqLength 5, SkipTrigger 50, SkipSize 512). A second profile at 1 MiB average
(min 512 KiB, max 8 MiB), tuned in W07.3.
**Reason:** 16 KiB is the largest size the SeqCDC paper tuned. Bigger chunks chunk faster, because more data is skipped.
Index cost: 65 536 chunks per GiB × 48 B ≈ 3 MiB per GiB of unique data, so 1 TiB of unique data needs about 3 GiB of RAM.
The 1 MiB profile has the same size as restic (and borg/kopia can be set to it). So a benchmark at 1 MiB measures code speed, not chunk-size effects.

## D3 — Bit-identical cut points everywhere
**Decision:** cuts must be identical for scalar, AVX2 and AVX-512, for any thread count, and through every binding.
**Reason:** if cuts depended on the machine, the same file backed up on two machines would not dedup. It also makes
testing simple: the scalar version is the oracle for all the others.

## D4 — Parallel chunking of one file: speculative segments + sequential stitch
**Decision:** split a large file into segments, chunk each segment in parallel starting at its own start, then walk the
true chain from the previous segment until it lands on one of the speculative cuts.
**Reason:** SeqCDC, RAM and FastCDC (with min-size skip) reset all their state at a cut. So once the true chain
and a worker agree on one cut, they agree on every cut after it. The result is exactly what sequential chunking would give.
If they never meet (example: all-zero data, where every cut is at max size), that segment is redone sequentially.
That is slower but still correct.

## D5 — Fuse chunking and hashing on 1 MiB blocks
**Decision:** each worker chunks a 1 MiB block and hashes its chunks while the block is still in L2 cache.
**Reason:** on 8 cores the limit is RAM bandwidth (~45 GB/s theoretical with one module), not compute. Separate passes
read every byte from DRAM twice. Fusing reads it once, which roughly doubles the headroom.

## D6 — BLAKE3 official C library, keyed mode for chunk IDs
**Decision:** use the official BLAKE3 C implementation (it has AVX-512 code). Chunk ID = keyed BLAKE3 with a repo key.
**Reason:** BLAKE3 was asked for and the official code is the fastest well-tested version. With a keyed hash, someone who
reads the repo cannot check whether it contains a known file by its plain hash. restic and borg do the same with keyed MACs.

## D7 — zstd, per-thread contexts, raw-store rule
**Decision:** zstd with one reused compression context per thread. Level chosen at gate G4 (expected: 1). If the
compressed size is ≥ 97% of the input, store the raw bytes.
**Reason:** zstd has the best speed-for-ratio. Reusing contexts avoids an allocation per chunk. Only unique chunks
are compressed. The 97% rule avoids paying decompression later for data that barely shrank.

## D8 — AES-256-GCM with a separate key per pack file
**Decision:** `pack_key = BLAKE3.derive_key("dedup v1 pack key", master_key ‖ pack_salt)`. `pack_salt` is 32 random
bytes stored in the pack header. Nonce = 96-bit blob counter inside the pack (0, 1, 2, …). Index, tree and snapshot
files are encrypted the same way, each with its own salt. The master key comes from a keyfile (no password KDF in v1).
**Reason — the random-nonce limit:** if every blob used one key with a *random* 96-bit nonce, NIST SP 800-38D limits
the key to about **2³² encryptions**, which keeps the chance of two blobs getting the same nonce below 2⁻³². A repeated
nonce under GCM leaks the XOR of the two plaintexts and lets an attacker forge messages. Example: at 16 KiB per chunk,
2³² chunks = 64 TiB of unique data. A long-lived repo can reach that.
**Reason — why per-pack keys fix it:** every pack gets a fresh key. Inside a pack, nonces are a counter, so they
can't repeat (a 64 MiB pack holds at most ~2²⁰ blobs). Two packs share a key only if their 256-bit salts collide (~2⁻¹²⁸).
Cost: one `derive_key` per 64 MiB pack, which is negligible. Encrypting per blob, not per pack, keeps random-access restore working.
**Reason for GCM:** all three competitors encrypt (restic can't turn it off), so a fair comparison needs encryption too.
With VAES on Zen 4 it costs well under 1 cycle per byte.

## D9 — 64 MiB append-only pack files, temp file + rename
**Decision:** chunks go into 64 MiB pack files with a trailer listing their contents. Plus sorted index files. Write to a temp
file, fsync, rename, fsync the directory.
**Reason:** large sequential writes are what NVMe is fastest at. The trailers mean the index can always be rebuilt
from the packs. Rename makes each pack appear all at once or not at all, so a crash never leaves a half pack. restic has proven this model.

## D10 — Sharded open-addressing index, full 32-byte keys
**Decision:** in-memory hash table split into shards by hash prefix. Each entry = 32 B chunk ID + 16 B location.
**Reason:** threads working on different shards never wait for one lock. Keeping the full 32-byte ID means two
different chunks can never be mistaken for the same one, so there is never false dedup.

## D11 — pread thread pool first, io_uring only if needed
**Decision:** read files with a pool of threads doing large `pread` calls. Build io_uring (optional week W19) only if gate G5 shows
reads below 90% of the measured disk ceiling.
**Reason:** the simplest design that can reach the target. io_uring adds a dependency, and its behaviour under WSL2 is a risk.

## D12 — Build system and dependencies
**Decision:** CMake + Ninja presets (release, debug, asan, ubsan, tsan). GCC 15 is the main compiler, Clang 21 for libFuzzer.
BLAKE3, zstd, GoogleTest and google/benchmark come through FetchContent, pinned to a tag and hash. OpenSSL comes from apt (libssl-dev).
**Reason:** pinned FetchContent gives the same build on any machine without system packages. OpenSSL is the exception because
the user agreed to a one-time sudo setup, and building it ourselves is slow and fragile.

## D13 — Benchmark harness in stdlib-only Python
**Decision:** `bench/e2e/*.py` uses only the Python standard library (tomllib, json, subprocess). Charts are SVG written by our own code.
Every report and criteria check is generated only from raw JSON results.
**Reason:** python3-venv/pip aren't installed. Zero dependencies means the harness always runs. JSON-only reports mean every number
can be traced back to the run that produced it.

## D14 — One check script per roadmap step
**Decision:** each roadmap step has `checks/WNN.S.sh` with its "done when" command. `scripts/check WNN.S` (or `WNN` for a whole week) runs them.
**Reason:** "done" is then a command result, not an opinion. The weekly buffer session re-runs all of the week's checks to catch regressions.

## D15 — 4 work steps + 1 buffer session per week; core/optional weeks
**Decision:** every week has 4 planned sessions plus 1 buffer session. Each week is marked core or optional.
**Reason:** some steps will take longer than planned. The buffer absorbs that without moving the whole schedule.
Optional weeks (W10, W19, W24) are the first things to cut. The core path alone takes 23 of the 26 weeks.

## D16 — Datasets: real + deterministic synthetic
**Decision:** D1 Linux kernel trees, D2 Debian raw cloud images, D3/D4 generated by `dedup-gen` from fixed seeds.
**Reason:** real data shows real-world dedup. Synthetic data is reproducible bit-for-bit, and lets us control the edit rate
and compressibility. D4 (32 GiB) is larger than the 16 GiB of physical RAM, so cold-cache runs really are cold.

## D17 — Public GitHub repo, Apache-2.0, no data in git
**Decision:** public repo `ChunChun451/dedup` under Apache-2.0. `data/`, `tools/bin/`, `results/raw/` and `build/` are never committed.
**Reason:** the user chose public. Apache-2.0 is permissive and fits BLAKE3 (CC0/Apache-2.0), zstd (BSD) and OpenSSL (Apache-2.0).
Datasets are large and have their own licenses, so only the scripts that fetch them and their sha256 manifests are committed.

## D18 — Chunker library `simdcdc` with C ABI, Rust and Go bindings
**Decision:** the chunkers live in `lib/simdcdc/` and build as a static and a shared library. They export only `simdcdc_*` C symbols.
Rust binding in `bindings/rust` (hand-written FFI, no bindgen). Go binding in `bindings/go` (cgo). Same repo.
Tag v0.1.0 = first library release (W09). v1.0.0 = the engine (W26).
**Reason:** a C ABI can be called from any language. Hand-written FFI avoids needing libclang for bindgen, and the API is small.
One repo keeps library and engine in sync, and the engine is the library's demo and end-to-end benchmark.

## D19 — Experiment E1: lane-parallel Gear for a faster FastCDC
**Decision:** try a FastCDC whose cuts are bit-identical to the reference but computed faster. Keep it only if it is ≥ 1.5× faster
on one core at the same parameters. Otherwise record the result under `## E1 — DROPPED` and delete the code.
**Reason:** the Gear hash is `h = (h << 1) + G[byte]` in 64 bits. After 64 more bytes, an old byte has been shifted out completely,
so the hash at any position depends only on the previous 64 bytes. Several lanes can therefore start anywhere, warm up on 64 bytes,
and produce exactly the same hash values as the sequential loop. That allows unrolled and SIMD versions with identical cuts.
The 1.5× bar is the user's rule: a smaller gain isn't worth the extra code to maintain.

## D20 — WSL2 configuration (.wslconfig)
**Decision:** `memory=12GB`, `processors=16`, `swap=4GB`, `sparseVhd=true`, `autoMemoryReclaim=disabled`.
**Reason:** 12 GiB leaves about 4 GiB for Windows. It fits the 4 GiB warm-cache datasets plus the index. `sparseVhd` lets the
virtual disk give space back to C: when repos are deleted, which matters with a 170 GB budget on a drive with 269 GiB (289 GB) free.
Disabling automatic memory reclaim stops WSL dropping the page cache in the middle of a warm-cache benchmark.

## D20b — No sparse VHD; reclaim space with `wsl --manage Ubuntu --compact` (replaces the sparseVhd part of D20)
**Decision:** `.wslconfig` keeps `memory=12GB`, `processors=16`, `swap=4GB` under `[wsl2]` and
`autoMemoryReclaim=disabled` under `[experimental]`, but does **not** set `sparseVhd`. After deleting large data
(datasets, benchmark repos), run in PowerShell: `wsl --shutdown`, then `wsl --manage Ubuntu --compact`.
**Reason:** checked on this machine (WSL 3.0.1, 2026-10-06):
- The WSL binaries read exactly `wsl2.memory`, `wsl2.processors`, `wsl2.swap`, `experimental.autoMemoryReclaim`,
  `experimental.sparseVhd`, so the section names above are right.
- `sparseVhd` only affects *newly created* disks, never the existing Ubuntu disk (`ext4.vhdx`, `fsutil` says NOT sparse).
- WSL 3.0.1 contains the message "Sparse VHD support is currently disabled due to potential data corruption".
  Forcing it needs `wsl --manage Ubuntu --set-sparse true --allow-unsafe`. Corruption of `ext4.vhdx` was reported on
  sparse disks (microsoft/WSL#10609). Losing the disk that holds all datasets and results is not worth automatic space return.
- Safe alternative: Linux mounts `/` with `discard`, so deleted blocks are already reported to the virtual disk, and
  `--compact` (calls Windows `CompactVirtualDisk`) then shrinks the file. That is manual, but safe. Revisit when a
  stable WSL release supports sparse disks again (3.0.2 pre-release re-adds them as experimental).

## D21 — One sudo script; cold-cache helper with the smallest possible permission
**Decision:** `scripts/setup-system.sh` (run once with sudo) installs every system package the roadmap needs:
fio, hyperfine, libssl-dev, pkg-config, and the build dependencies of `perf` for W05.1 (flex, bison, libelf-dev,
libdw-dev, libtraceevent-dev, libcap-dev). It also installs `/usr/local/sbin/dedup-drop-caches` (root-owned, mode 755)
and a sudoers rule that lets only this user run only that file, with no arguments, without a password.
`scripts/check-env.sh` verifies all of it without sudo.
**Reason:** the user agreed to type the password once, so everything that needs root goes into this one script now,
including packages needed months later. Cold-cache benchmarks must empty the Linux file cache before every
run, which needs root. A root-owned helper that takes no arguments is the narrowest permission that works:
the user cannot edit the file, and sudo refuses any extra arguments (the `""` in the rule).
Example: `sudo -n /usr/local/sbin/dedup-drop-caches` works without a password, but `sudo -n ls` still asks for one.

## D22 — dedup-gen: ChaCha20, 1 MiB independent blocks, integer-only, compiler-independent
**Decision:** `tools/gen` generates D3/D4 data. Random bytes are a ChaCha20 keystream (RFC 8439 block function,
checked against the RFC test vector) keyed by the seed. Text, structured (log lines) and mixed data are built in
independent 1 MiB blocks, each driven by its own ChaCha20 stream (seed, mode, block number). Only integer math is
used (Zipf weights 2³²/(r+1), no `pow`/`log`). Every random draw is its own statement. `mutate` makes edits of
1–8192 bytes (insert/delete/overwrite, ⅓ each) at uniform gaps in [0, 2G), G = 4096.5 ÷ rate. Golden BLAKE3 hashes are
in `tests/gen_golden_test.cpp`; `scripts/gen-crosscheck.sh` checks that a Clang build writes the same bytes as GCC.
**Reason:** datasets must be identical on every machine and every rerun, or results are not comparable. Independent
blocks give the same file with 1 or 16 threads (random data: 1.8 GB/s, as fast as the disk writes). ChaCha20 is
standard and testable. Floating point and the order of draws inside one C++ expression can differ between compilers
and libraries. That happened here: the first version's structured and mixed data differed between GCC and Clang,
because `snprintf(…, rng.Below(…), rng.Below(…))` evaluates its arguments in an unspecified order. Edits of up to
8 KiB, one per ~820 KB on average at 0.5%, touch a few percent of 16 KiB chunks, like real file updates. Spreading
0.5% as single bytes would touch nearly every chunk. Measured zstd-1 ratios: random 1.00, text 2.76, structured 3.52,
mixed 1.96. SPEC's "~3:1" for D3 was corrected to the measured ~2:1 (a description, not a target).

## D23 — Datasets: pinned sources, release builds for D2, one digest per file or tree
**Decision:** `scripts/datasets/sources.txt` pins every download: 13 kernel tarballs (sha256 from kernel.org's
`sha256sums.asc`) and 10 Debian 13 genericcloud **release** builds 2026-01-12 … 2026-05-01 (sha512 from each build's
`SHA512SUMS`). D2 downloads the 226 MB `.tar.xz` per build, not the 3 GiB `.raw`, and extracts `disk.raw` sparse.
Layout: `data/d1/linux-6.X`, `data/d2/<build>/disk.raw`, `data/d3/v{0,1,2}/f0` (mixed seed 3, 16 GiB; mutate seeds
31, 32), `data/d4/big0` (random seed 4, 20 GiB) + `big1` (seed 5, 12 GiB), `data/d1-small/linux-6.12`,
`data/d3-small/f0` (mixed seed 3, 4 GiB), `data/d4-small/f0` (random seed 44, 4 GiB). `scripts/datasets/manifest.txt`
holds one SHA-256 per dataset file or tree (`treehash.py`: sorted paths, type, permission bits, size or link target,
file hash; no owners or times). It is recorded once with `verify.sh --record` and committed.
**Reason:** Debian deletes daily images after a few months: the oldest daily on 2026-10-06 is from June 2026. Release
builds have been kept since August 2025, so D2 stays reproducible until the end of the project. Consecutive builds
two weeks apart are realistic VM-image updates. The `.tar.xz` route downloads 2.3 GB instead of 32 GB. One digest per
tree, instead of one line per file (~1.2 M lines for D1), keeps the manifest small enough to commit. Leaving out owners
and times makes the digest identical on any machine. D4 is split 20 + 12 GiB so that W15.3 has a single file larger than
RAM (`big0`). Measured: verifying the three small sets takes ~1 min, mostly opening D1-small's ~90k files (36 s).

## D24 — Benchmark harness design (bench/e2e/run.py)
**Decision:** a tool is described by `bench/tools/<tool>.toml` (version command, init/backup/restore command lists
with `{src}`, `{repo}`, `{target}`, `{bin}`, `{secrets}`, `{config_args}` placeholders, and named configs). A cell
is (tool, config, dataset, workload, rep). For each cell: fresh repo (or a reused one-snapshot repo for restores),
unmeasured warm-up run for `*-small` datasets or dropped page cache for full ones, a 2 s calibration (`calib`;
> 5% off the session baseline → 60 s cooldown, up to 3 times), then the command under `/usr/bin/time -v`, followed by
`sync` inside the timed span. Recorded: wall, user/sys CPU, peak RSS, filesystem bytes, repo size, virtual-disk growth,
Windows disk counters sampled about once per second (stop-file controlled) and the drive-state label (SPEC 6.12),
and a tree-digest check of the first restore per (tool, config, dataset). Tool order rotates per rep (cyclic Latin
square). One JSON file per cell, plus `session.json`, `plan.json`, `summary.json` (median, min, max, CoV, CoV > 5% flag,
drive states).
**Reason:** commands as argument lists (no shell) avoid quoting bugs; one file per cell makes a long night resumable
and auditable; everything SPEC section 6 asks for is recorded where it happens, so reports never need a re-run.
Calibration repeats within ±2.5% here (1.122–1.154), so 5% separates real slowdowns from noise.

## D25 — io_ceiling: parallel O_DIRECT reads in path order, preallocated parallel writes, two modes
**Decision:** `tools/io_ceiling` reads every regular file under the inputs in sorted path order, split into segments of
up to 16 MiB that 8 threads read with 1 MiB O_DIRECT requests. It writes the requested bytes into a new file in
`data/scratch/io_ceiling`: `posix_fallocate` first, then 8 threads with 1 MiB O_DIRECT writes, then `fdatasync`, all
timed. It runs a *sequential* mode (read, then write) and an *overlapped* mode (both at once), each after dropping
the page cache. The ceiling is the faster one (G1b).
**Reason:** a ceiling must be the best a tool could possibly do on this disk. One request at a time would understate the
disk, so several threads keep requests in flight. Without `fallocate`, 8 threads extending a new file measured
0.6–0.75 GB/s instead of ~1.8, because ext4 allocates blocks during each write. A tool could preallocate too, so the
ceiling does. First measurements on D4-small (4.29 GB read): with 1 GiB written, ceiling 1.18–1.22 s, overlapped;
with 4 GiB written, 4.11 s (writes at ~1.05 GB/s, the drive's slow state after a day of tests).

---

## G1 — Targets recalibrated to the measured limits (W01, 2026-10-06)
**Decision:** SPEC.md v1.1 changes P1, P3a, P5a and P9, adds the disk-state rules (SPEC §6.12), and drops the
20 GiB flush file. All other targets stay. This was the one allowed revision; the numbers are now frozen.

**Measured limits** (GB = 10⁹ bytes; sources: `results/hw/baseline.json`, `results/hw/g1-investigation.json`):
| Limit | Value | How measured |
|---|---|---|
| RAM read, 1 / 16 threads (`M1` / `M16`) | 20.5–23.0 / 22.9–23.4 GB/s | `membw`, 4 GiB buffer, best of 7; saturates already at 2 threads (one memory module) |
| Disk read `R` | 5.9–6.3 GB/s (baseline: 6.2) | fio, 32 GiB, O_DIRECT, 1 MiB, QD32; 5 runs |
| Disk write `W`, fast state, space the virtual disk already has | 1.81–1.86 GB/s (baseline: 1.81) | 4 × 32 GiB (W1–W3 + rested baseline); Windows saw the NVMe write the same bytes |
| Disk write, slow state (after ~250 GB written in 40 min) | ~1.0 GB/s | Windows per-second NVMe counter; 5 min idle refilled only ~17 GB of fast writes |
| Disk write while `ext4.vhdx` grows | 1.26 GB/s | NVMe wrote 44.2 GB for 34.4 GB of data |
| Native Windows write (no WSL), 8 × 64 MiB in flight | 2.13 GB/s | so WSL costs ~15%; the drive itself does not sustain its 7 GB/s rating |
| Warm re-read (4 GiB in page cache → our buffer), 8 readers | median 13.6–14.2 GB/s (two sets of 10: 12.7–15.8) | fio, buffered, `--invalidate=0` |
| BLAKE3, one core, 16 KiB inputs, AVX-512 | 3.76 GB/s (3.50 GiB/s) | `blake3_simd_degree()` = 16 |
| Windows caches the virtual disk? | no (ratio 0.86–0.96) | 2 GiB O_DIRECT read-back vs `R` |

**Every target against those limits:**
| Target | What it needs | Bound by | Possible as written? | G1 change |
|---|---|---|---|---|
| P1 chunk ≥ 20 GB/s, 1 core, 1 GiB in RAM | read 1 GiB from RAM at 20 GB/s while chunking | RAM, 1 thread: 20.5–23.0 GB/s | Not proven impossible, but at 87–98% of `M1` it measures RAM, not our code | Split (agreed): **P1a** L2-resident 1 MiB buffer ≥ 20 GB/s (AVX2 ≥ 12), pure compute, not lowered; **P1b** 1 GiB from RAM ≥ **0.85 × `M1`** (17.4–19.6 GB/s). Report both |
| P2 chunk+hash ≥ 15 GB/s, 16 threads, in RAM | 4.0 cores of BLAKE3 + 0.75 core of chunking; 15 of 23 GB/s RAM | BLAKE3 3.76 GB/s/core; RAM 23 GB/s (65% used) | Yes (tight on RAM) | none |
| P3a warm, null store, all-duplicate ≥ 10 GB/s | copy every byte out of the page cache, then chunk + hash it | warm re-read 13.6–14.2 GB/s (copy alone, no work) | **Yes**: 10 is below the measured copy limit. (My ~9 GB/s was an estimate, not a measurement; see below) | Made relative, not lowered: ≥ **0.7 × warm re-read median** of the same session (9.5–9.9 GB/s today). If W17 misses it, the gap plan says so. mmap stays a later experiment |
| P3b same, unique incompressible ≥ 5 GB/s | copy + chunk + hash + zstd attempt + AES-GCM, nothing written | CPU: estimate 4.6–5.7 GB/s (zstd speed on random data unmeasured until W11.3); not bound by disk | Yes, unverified; a cheap "does it compress?" check before zstd gives ~7 GB/s | none |
| P3c same, compressible 3:1 ≥ 2.5 GB/s | the above + zstd level 1 on every byte | CPU: zstd ~0.6 GB/s/core → estimate ~3 GB/s on 8 cores | Yes (tight), unverified until W11 | none |
| P4a cold first backup D2/D4 ≤ 1.15 × `io_ceiling` | read input + write repo on one disk | R 6.3 / W 1.8, but the target is relative | Yes, if our run and `io_ceiling` see the same disk state | SPEC §6.12 rules make that true |
| P4b cold re-backup, no files cache ≤ 1.10 × read time | chunk + hash at ≥ 5.7 GB/s from O_DIRECT reads | R 6.3 GB/s; needs ~1.5 cores of hashing | Yes | none |
| P5a warm first backup ≥ 3 × fastest competitor, every dataset | on **D4-small** every tool must write all ~4.3 GB of random data, and timing includes `sync` | **W 1.81 GB/s**: D4-small warm ceiling = **1.59 GB/s for any tool** | **No** on D4-small once the fastest competitor passes 1.59 ÷ 3 = 0.53 GB/s (measured limit W) | ≥ **min(3 × fastest, 0.87 × our warm ceiling)**. On D4-small today: 0.87 × 1.59 = **1.38 GB/s**, so the 3× rule binds only while the fastest competitor is below 0.46 GB/s |
| P5b cold first backup ≥ min(2 × fastest, 0.87 × ceiling) | **D4**: read 34.4 GB + write ~34.6 GB on one disk | W: ≥ 19 s of writing → ≤ ~1.8 GB/s for every tool | Yes: the cap is already there; on D4 it is effectively "≥ 0.87 × `io_ceiling`" | none; valid only with §6.12 (a tool measured in the slow state would look 45% slower) |
| P5c cold restore ≥ min(2 × fastest, 0.85 × `W`) | D4: write 34.4 GB | W 1.8 GB/s | Yes (cap) | `W` = fast-state value; §6.12 rules apply |
| P5d unchanged incremental, files cache on ≤ fastest | stat ~1M files (D1) | metadata, none of the measured limits | Yes | none |
| P6 repo size ≤ smallest competitor; D4 ≤ 1.01 × input | per-chunk overhead ≈ 16 B tag + ~32 B header + 48 B index per 16 KiB ≈ 0.6% | not a speed target | Yes | none |
| P7 RSS ≤ 1.5 GiB + 64 B/chunk | D4: 2.1 M chunks → +134 MB | 11.7 GiB visible RAM | Yes | none |
| P8 SeqCDC dedup within 3% of FastCDC | data-dependent | not a speed target | Decided at G2 | none |
| P9 1 MiB chunks, warm first backup ≥ 3 × fastest (same config) | same write floor as P5a | W 1.81 GB/s | **No** on D4-small (same reason as P5a) | same cap as P5a, with our 1 MiB-chunk repo bytes (D4-small: also ≈ 1.38 GB/s) |
| P10 bindings ≥ 95% of C++ | same compute through FFI | CPU | Yes | none |
| P11 E1 fast FastCDC ≥ 1.5× (keep/drop) | Gear hash at a few GB/s | CPU, far below RAM | Yes (an experiment; either outcome is allowed) | none |

**Official baseline** (`results/hw/baseline.json`, rested 20 min, probe 1.94 GB/s average / 1.62 last 5 s):
R 6.20, W 1.81, M1 20.5, M16 22.9, warm re-read median 13.6 GB/s, no host caching, no virtual-disk growth.

**Other changes:**
- SPEC §6.6: no 20 GiB flush file. Windows does not cache the virtual disk, so dropping the Linux page cache is enough.
- SPEC §6.12 (new), disk-state rules (see "Probe cost" below for why there is no per-cell probe): grow the virtual
  disk once before the campaign and never compact it during the campaign; one rested 16 GiB write probe per session;
  Windows' per-second disk counters classify every write-heavy cell as fast or slow drive state; tool order is a Latin
  square; a comparison whose two sides ran in different drive states is re-run (capped per night); a cell during which
  `ext4.vhdx` grew > 1 GB is re-run; warm ceilings are medians of 10 runs.
- `bench/hw/run.py` (the baseline, run once per session) uses the rested probe, logs the write speed per second, and
  records virtual-disk growth.

**Warm ceiling, exact definition (P5a, P9):** for one dataset and one tool,
`T_warm = input bytes ÷ warm re-read median + repo bytes that tool wrote ÷ W`, and warm ceiling = input bytes ÷ `T_warm`.
P5a and P9 use our own repo bytes; `W` and the warm re-read median come from the hardware baseline of the same session.
D4-small today: 4.295 GB ÷ 13.59 GB/s + 4.32 GB ÷ 1.812 GB/s = 0.316 s + 2.385 s = 2.701 s → **1.59 GB/s**;
0.87 × 1.59 = **1.38 GB/s**. (4.32 GB = 4.295 GB of random data + ~0.6% per-chunk overhead; the real value is measured.)

**Estimates vs measurements (G1 rule: only lower what measurements prove impossible):**
- The "~9 GB/s" for P3a was arithmetic: each of 8 fio readers copies at 14.2 ÷ 8 = 1.78 GB/s (measured), BLAKE3
  3.76 GB/s per core (measured), chunking 20 GB/s per core (a target, not measured); assuming no overlap and no SMT gain,
  1 ÷ (1/1.78 + 1/3.76 + 1/20) = 1.14 GB/s per core × 8 = 9.1 GB/s. Both assumptions are pessimistic, so it proves
  nothing. P3a therefore keeps ~10 GB/s, tied to the measured copy speed (0.7×).
- The 0.75 first proposed for P1b was reasoning only (one core cannot fully overlap RAM loads with compute), not a
  measurement, so P1b uses 0.85.

**Probe cost — why the per-cell probe was dropped:** the full matrix has about **300 write-heavy cells** (≥ 4 GiB written):
cold first backups of D3 and D4 (12 tool-configs × 2 × 5 reps = 120, ~22 GB each), warm first backups of D4-small
(60, ~4.3 GB each) and cold restores of D3 and D4 (120, ~26 GB each). They write ~6.0 TB themselves.
| Per-cell 16 GiB probe rule | Cost |
|---|---|
| Probe writes, every probe passes at once | 5.15 TB (almost doubles the matrix's writes) |
| Probe writes, worst case (6 tries per cell) | 30.9 TB |
| Waiting, one 10-min wait per cell | 50 h |
| Waiting, worst case (5 waits per cell) | 250 h |
| Rest needed to run every cell in the fast state (~17 GB refilled per 5 min) | ~30 h |
One night is ~10 h, so no rule can keep all 300 cells on a rested drive. The rule instead makes the drive state visible
and keeps comparisons like-for-like: one probe per session (~17 GB), free counters, and re-runs only for comparisons
whose sides ran in different states (at most 5 per night, ~1.5 TB worst case; the rest move to the second night, since
W23.1 and W25.1 already plan two nights). Not chosen: SNIA-style "precondition everything to the slow steady state",
because a real nightly backup usually starts on a rested drive.

**Reason — write speed depends on history:** in reused space, three 32 GiB writes ran at 1.81–1.86 GB/s, with or
without 3 minutes idle before them. After ~250 GB of writes in 40 minutes, the same write ran at ~1.0 GB/s and Windows
saw the NVMe itself at ~1.0 GB/s. 5 minutes of idle refilled only ~17 GB of fast writes (an 8 GiB probe passed at
2.05 GB/s; the 32 GiB write after it fell back to ~1.0). So the drive's recent history decides the speed. A fair
comparison must start every write-heavy cell from the same rested state.

**Reason — the warm re-read outlier:** one run gave 8.8 GB/s. These did not reproduce it: thread pinning (median
14.3 vs 14.2), CPU clock (±4%), file not fully cached (100% cached every time), heavy writes just before (24 runs,
10.2–17.0) and first pass after caching (weak, ≤ 20%). About 80 runs later the lowest 8-reader run was 9.0. Cause unknown;
most likely a one-off interruption from Windows: the rested baseline had one single-reader run at 0.63 GB/s while the
other nine were 5.5–7.8. Hence the median-of-10 rule.

**Facts corrected at G1:** C: free space was 269 GiB (289 GB) at session start, not "269 GB". Before W01.4 it was 400 GiB,
because something on the Windows side freed ~131 GiB. Now it is 384.7 GB (358 GiB), after the virtual disk grew by ~37 GB for the tests.

## G1b — Correction to G1: warm ceiling uses max(), io_ceiling measures two modes (2026-10-06)
**Decision:** replaces the warm-ceiling formula in G1 (the P5a and P9 rows and "Warm ceiling, exact definition").
`T_warm = max(input bytes ÷ warm re-read median, repo bytes that tool wrote ÷ W)`, not their sum. For P4a and P5b,
`io_ceiling` (W02.4) measures a *sequential* mode (read all input, then write the repo bytes) and an *overlapped* mode
(read and write at the same time on the same disk); the ceiling is the faster of the two.
**Reason:** a warm read comes from RAM, not the SSD, and a pipeline reads the next piece while the previous one is being
written. So the minimum time is the longer of the two, not their sum. D4-small today:
max(0.316 s, 2.385 s) = 2.385 s → warm ceiling **1.80 GB/s** (G1 said 1.59). The P5a/P9 cap becomes
0.87 × 1.80 = **1.57 GB/s** (G1 said 1.38), and the "3 × fastest" part binds while the fastest competitor is below
0.52 GB/s. For cold runs, reads and writes share one SSD, so whether overlap helps is a measurement, not an
assumption: hence both modes. Both changes raise the ceiling, so the targets get stricter, never easier, which keeps
the G1 rule (only measurements may lower a target). The user spotted the sum.

## (pending) G2 — SeqCDC vs FastCDC dedup (W04)
_Pending._

## (pending) E1 — Fast FastCDC experiment (W07)
_Pending. When decided, rename this header to `## E1 — KEPT` or `## E1 — DROPPED`._

## (pending) G3 — Default chunker for 16 KiB and 1 MiB (W07)
_Pending._

## (pending) G4 — Default zstd level (W11)
_Pending._

## (pending) G5 — io_uring needed? (W18)
_Pending._
