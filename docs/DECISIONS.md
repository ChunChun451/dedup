# Decisions

Every technical decision and its reason. New decisions are appended. A changed decision gets a new entry that
points to the old one; old entries are never deleted. Gates (G1–G5) and experiments (E1) start as `## (pending) Gx` and are
renamed to `## Gx — <result>` when decided, because the roadmap checks look for `^## Gx`.

## Machine facts these decisions are based on (measured 2026-10-06)
- AMD Ryzen 7 8845HS (Zen 4), 8 cores / 16 threads, AVX-512 (F/BW/VL/DQ/VBMI/VBMI2/VPOPCNTDQ…), VAES, SHA-NI.
- L2 1 MiB per core, L3 16 MiB.
- 16 GiB DDR5-5600, one module (probably single-channel, ~45 GB/s theoretical). WSL2 sees 7.4 GiB before W01.3.
- SK Hynix PC801 1 TB NVMe PCIe 4.0, which Linux sees only through a WSL2 virtual disk (ext4). C: has 269 GB free.
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
virtual disk give space back to C: when repos are deleted, which matters with a 170 GB budget on a drive with 269 GB free.
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

---

## (pending) G1 — Hardware recalibration (W01)
_Pending._

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
