"""Disk-state helpers shared by the hardware baseline and the benchmark harness (DECISIONS.md G1).

Two effects found at G1 make write speed depend on history, not only on the code being measured:
1. After a lot of recent writing (~250 GB in 40 min), the SSD drops from ~1.8 GB/s to ~1.0 GB/s (its fast write
   cache is used up). Idle time refills it only gradually: 5 min of idle gave back ~17 GB of fast writes.
   Rule: each session starts rested and runs one 16 GiB write probe that must still be fast in its last seconds
   (an 8 GiB probe passed at 2.05 GB/s and the 32 GiB write after it still fell to 1.0 GB/s). There is no probe per
   cell (too costly, see DECISIONS.md G1); instead the harness labels each cell fast/slow from Windows disk counters.
2. When the WSL virtual-disk file (ext4.vhdx) has to grow, Windows writes ~30% extra bytes and the write speed
   falls to ~1.25 GB/s. Rule: the virtual disk is grown once before measuring, and a measurement during which
   it grew by more than 1 GB is invalid.
"""
import glob
import json
import os
import pathlib
import statistics
import subprocess
import time

GiB = 1 << 30
PROBE_GIB = 16
TAIL_S = 5  # judge the probe by its last TAIL_S one-second samples
FAST_GBPS = 1.5  # fast state averages ~1.8 GB/s; slow state ~1.0 GB/s
WAIT_S = 600
MAX_TRIES = 6


def vhdx_bytes():
    """Size in bytes of the WSL virtual-disk file, or None if it can't be found."""
    files = glob.glob("/mnt/c/Users/*/AppData/Local/wsl/*/ext4.vhdx")
    return os.stat(files[0]).st_size if len(files) == 1 else None


def write_probe(work_dir, raw_dir=None):
    """Write PROBE_GIB with O_DIRECT (1 MiB, QD32). Returns (average GB/s, median GB/s of the last TAIL_S seconds).
    The probe file is deleted afterwards."""
    path = pathlib.Path(work_dir) / "probe.dat"
    raw = pathlib.Path(raw_dir or work_dir)
    out = raw / "probe.json"
    for old in raw.glob("probe_bw*.log"):
        old.unlink()
    try:
        subprocess.run(["fio", "--name=probe", f"--filename={path}", f"--size={PROBE_GIB}g", "--rw=write",
                        "--bs=1m", "--ioengine=libaio", "--iodepth=32", "--direct=1", "--end_fsync=1",
                        f"--write_bw_log={raw / 'probe'}", "--log_avg_msec=1000",
                        "--output-format=json", f"--output={out}"], check=True)
        avg = json.loads(out.read_text())["jobs"][0]["write"]["bw_bytes"] / 1e9
        log = next(raw.glob("probe_bw*.log")).read_text().splitlines()
        secs = [int(l.split(",")[1]) * 1024 / 1e9 for l in log if l.strip()]
        return avg, statistics.median(secs[-TAIL_S:])
    finally:
        path.unlink(missing_ok=True)


def wait_for_fast_writes(work_dir, raw_dir=None, log=print, initial_idle_s=0):
    """Rest initial_idle_s, then probe until the probe's average and its last TAIL_S seconds are both >= FAST_GBPS;
    wait WAIT_S between tries. Returns the list of probe results as [average, tail] pairs."""
    if initial_idle_s:
        log(f"  resting the disk for {initial_idle_s} s before the first probe")
        time.sleep(initial_idle_s)
    tries = []
    for i in range(MAX_TRIES):
        avg, tail = write_probe(work_dir, raw_dir)
        tries.append([round(avg, 3), round(tail, 3)])
        log(f"  write probe {i + 1}: average {avg:.2f} GB/s, last {TAIL_S} s {tail:.2f} GB/s")
        if avg >= FAST_GBPS and tail >= FAST_GBPS:
            return tries
        if i + 1 < MAX_TRIES:
            log(f"  disk is in its slow write state; idle {WAIT_S} s")
            time.sleep(WAIT_S)
    raise RuntimeError(f"disk stayed below {FAST_GBPS} GB/s after {MAX_TRIES} probes: {tries}")
