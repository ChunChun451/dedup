# dedup — notes for Claude

C++20 deduplication engine + `simdcdc` chunking library. Read `SPEC.md` (what and targets), `ROADMAP.md`
(weekly steps) and `docs/DECISIONS.md` (every decision with its reason) before changing anything.

## Build and test
```bash
cmake --preset release && cmake --build --preset release   # binaries in build/release
ctest --preset release                                     # all tests
ctest --preset release -R smoke                            # one group (regex)
scripts/check W01.2                                        # one roadmap step's "done when"
scripts/check W01                                          # every step of a week
```
Other presets: `debug`, `asan`, `ubsan`, `tsan` (same commands, different preset name).

## Main rules
- When explaining to the user, use simple English, short sentences, and a concrete example with real numbers.
- Claude makes the technical decisions. Every decision goes into `docs/DECISIONS.md` with its reason.
  Never edit an old decision; add a new one (e.g. D20b) that says what it replaces.
- Gates and experiments start as `## (pending) G1 …`. Rename to `## G1 — <result>` only when actually decided,
  because roadmap checks grep for `^## G1`.
- A roadmap step is done only when its `checks/WNN.S.sh` passes. Write that file in the same step.
- Chunk cut points must be bit-identical for scalar, AVX2, AVX-512, any thread count and every binding (D3).
  The scalar version is the test oracle.
- Never read or commit `data/` (datasets), `tools/bin/` or `results/raw/`.
- Third-party code comes only through pinned FetchContent (tag + SHA256) in `cmake/Dependencies.cmake`.
- Our code builds with `-Wall -Wextra -Wpedantic -Werror` (target `dedup_warnings`).
- End of every session: add an entry to `docs/PROGRESS.md`, add new concepts to `docs/LEARN.md`, commit, `git push`.

## Machine
Ryzen 7 8845HS (Zen 4, 8C/16T, AVX-512), WSL2 with 11 GiB RAM visible, ext4 virtual disk on a PCIe 4.0 NVMe.
No passwordless sudo: anything needing root goes into `scripts/setup-system.sh`, which the user runs.
