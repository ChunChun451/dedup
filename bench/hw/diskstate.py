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
import datetime
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


# ---------- Windows disk counters during a benchmark cell (SPEC.md 6.12c) ----------

WIN_TEMP_LINUX = "/mnt/c/Users/{user}/AppData/Local/Temp"
DRIVE_LIMITED_QUEUE = 8          # provisional, calibrated in W22.3
DRIVE_LIMITED_WRITE_BYTES = 256 << 10
DRIVE_LIMITED_MIN_SECONDS = 5
FAST_STATE_GBPS = 1.4
WRITE_HEAVY_BYTES = 4 * GiB


class WinDiskCounters:
    """Samples Windows' physical-disk write counters about once per second until stop() is called.

    Uses a stop file instead of killing PowerShell, because killing the WSL side can leave powershell.exe running."""

    COUNTERS = ["\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                "\\PhysicalDisk(_Total)\\Avg. Disk Write Queue Length",
                "\\PhysicalDisk(_Total)\\Avg. Disk Bytes/Write"]

    def __init__(self, tag):
        user = os.environ.get("DEDUP_WIN_USER") or os.path.basename(glob.glob("/mnt/c/Users/*/AppData/Local/wsl")[0]
                                                                      .rsplit("/AppData", 1)[0])
        self.stop_linux = pathlib.Path(WIN_TEMP_LINUX.format(user=user)) / f"dedup-counters-{tag}.stop"
        self.stop_win = f"C:\\Users\\{user}\\AppData\\Local\\Temp\\dedup-counters-{tag}.stop"
        self.proc = None

    def start(self):
        self.stop_linux.unlink(missing_ok=True)
        counters = ",".join(f"'{c}'" for c in self.COUNTERS)
        script = (f"$c = @({counters}); $first = $true; "
                  f"while (-not (Test-Path '{self.stop_win}')) {{ "
                  "$s = Get-Counter -Counter $c -SampleInterval 1 -MaxSamples 1; "
                  "$v = ($s.CounterSamples | ForEach-Object { $_.CookedValue }) -join ','; "
                  "if ($first) { [Console]::Out.WriteLine('READY'); $first = $false }; "
                  "[Console]::Out.WriteLine($s.Timestamp.ToUniversalTime().ToString('o') + ',' + $v) }")
        self.proc = subprocess.Popen(["powershell.exe", "-NoProfile", "-Command", script], cwd="/mnt/c",
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        # Wait for the first sample, so the measured command never runs unobserved.
        line = self.proc.stdout.readline()
        if line.strip() != "READY":
            raise RuntimeError("Windows disk counters did not start")
        return self

    def stop(self):
        """Stop sampling; return a list of (unix_time, write_bytes_per_s, write_queue, bytes_per_write)."""
        self.stop_linux.touch()
        out, _ = self.proc.communicate(timeout=30)
        self.stop_linux.unlink(missing_ok=True)
        samples = []
        for line in out.splitlines():
            parts = line.strip().split(",")
            if len(parts) != 4:
                continue
            ts = datetime.datetime.fromisoformat(parts[0]).timestamp()
            samples.append((ts, float(parts[1]), float(parts[2]), float(parts[3])))
        return samples


def classify_drive_state(samples, t0, t1):
    """SPEC.md 6.12c: label a cell 'fast', 'slow' or 'n/a' from Windows disk samples taken between t0 and t1."""
    inside = [s for s in samples if t0 <= s[0] <= t1 + 1.5]
    written = sum(s[1] for s in inside)  # ~1 s per sample
    limited = [s[1] for s in inside if s[2] >= DRIVE_LIMITED_QUEUE and s[3] >= DRIVE_LIMITED_WRITE_BYTES]
    res = {"samples": len(inside), "nvme_written_gb": round(written / 1e9, 2), "drive_limited_s": len(limited),
           "median_limited_gbps": round(statistics.median(limited) / 1e9, 3) if limited else None,
           "max_queue": round(max((s[2] for s in inside), default=0), 1)}
    if written < WRITE_HEAVY_BYTES or len(limited) < DRIVE_LIMITED_MIN_SECONDS:
        res["state"] = "n/a"
    else:
        res["state"] = "fast" if statistics.median(limited) >= FAST_STATE_GBPS * 1e9 else "slow"
    return res
