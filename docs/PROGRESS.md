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
- Next: W01.3 check, then W01.4 (fio + memory bandwidth baseline).
