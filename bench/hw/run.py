#!/usr/bin/env python3
"""Measure this machine's hardware ceilings (roadmap W01.4) and write results/hw/baseline.json.

Ceilings (SPEC.md section 3), all in GB/s (1 GB = 10^9 bytes):
  W   sequential write, fio, O_DIRECT, 1 MiB blocks, queue depth 32, 32 GiB file
  R   sequential read of the same file, same settings
  M1  / M16   memory read bandwidth with 1 / 16 threads (bench/hw/membw)
Also measured:
  host cache test: a small file read with O_DIRECT right after writing it. If it is much faster than R,
    Windows is caching the WSL virtual disk, and cold-cache runs need the flush file (SPEC.md 6.6).
  warm read: reading a file that is already in Linux's page cache (the ceiling for warm-cache benchmarks).
Raw fio output goes to results/raw/hw/<timestamp>/ (not committed).
"""
import argparse
import datetime
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
GiB = 1 << 30


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def drop_caches():
    subprocess.run(["sudo", "-n", "/usr/local/sbin/dedup-drop-caches"], check=True)


def fio(raw_dir, name, path, size_gib, rw, direct=True, numjobs=1, extra=()):
    out = raw_dir / f"{name}.json"
    cmd = ["fio", f"--name={name}", f"--filename={path}", f"--size={size_gib}g", f"--rw={rw}",
           "--bs=1m", "--ioengine=libaio", "--iodepth=32" if direct else "--iodepth=1",
           f"--direct={1 if direct else 0}", f"--numjobs={numjobs}", "--group_reporting",
           "--output-format=json", f"--output={out}", *extra]
    if numjobs > 1:
        cmd.append(f"--offset_increment={size_gib * GiB // numjobs}")
        cmd[cmd.index(f"--size={size_gib}g")] = f"--size={size_gib * GiB // numjobs}"
    t0 = time.monotonic()
    subprocess.run(cmd, check=True)
    wall = time.monotonic() - t0
    job = json.loads(out.read_text())["jobs"][0]
    side = job["write"] if rw == "write" else job["read"]
    return {
        "gbps": round(side["bw_bytes"] / 1e9, 3),
        "wall_gbps": round(side["io_bytes"] / wall / 1e9, 3),
        "io_bytes": side["io_bytes"],
        "error": job["error"],
    }


def membw(threads):
    exe = ROOT / "build/release/bench/hw/membw"
    return json.loads(sh([str(exe), str(threads), "4"]))


def windows_power():
    """Return AC/battery state and power mode as seen by Windows (best effort)."""
    ps = ("Add-Type -AssemblyName System.Windows.Forms;"
          "[System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus;"
          "(Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes')"
          ".ActiveOverlayAcPowerScheme")
    try:
        out = sh(["powershell.exe", "-NoProfile", "-Command", ps], cwd="/mnt/c", timeout=60).split()
        overlay = {"ded574b5-45a0-4f42-8737-46345c09c238": "best_performance",
                   "961cc777-2547-4f9d-8174-7d86181b8a7a": "best_power_efficiency",
                   "00000000-0000-0000-0000-000000000000": "balanced"}
        return {"power_line": out[0], "power_mode": overlay.get(out[1] if len(out) > 1 else "", "unknown")}
    except Exception as e:  # noqa: BLE001 - recorded, not fatal
        return {"power_line": "unknown", "power_mode": "unknown", "error": str(e)}


def machine_info():
    cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
    mem_kib = int(next(l.split()[1] for l in open("/proc/meminfo") if l.startswith("MemTotal")))
    return {
        "cpu": cpu,
        "nproc": os.cpu_count(),
        "mem_total_gib": round(mem_kib / (1 << 20), 2),
        "kernel": os.uname().release,
        "fio": sh(["fio", "--version"]).strip(),
        **windows_power(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=str(ROOT / "data/scratch/hw"), help="where the test files go (ext4 disk)")
    ap.add_argument("--size-gib", type=int, default=32, help="big file size; must exceed the 16 GiB of host RAM")
    ap.add_argument("--small-gib", type=int, default=2, help="file size for the host-cache test")
    ap.add_argument("--out", default=str(ROOT / "results/hw/baseline.json"))
    a = ap.parse_args()

    stamp = datetime.datetime.now().astimezone().strftime("%Y%m%dT%H%M%S")
    raw = ROOT / "results/raw/hw" / stamp
    raw.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(a.dir)
    work.mkdir(parents=True, exist_ok=True)
    need = (a.size_gib + a.small_gib + 1) * GiB
    if shutil.disk_usage(work).free < need + 30 * 10**9:
        sys.exit(f"not enough free space in {work}: need {need / 1e9:.0f} GB + 30 GB margin")
    big, small, warm = work / "big.dat", work / "small.dat", work / "warm.dat"

    res = {"schema": 1, "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
           "git_sha": sh(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).strip(),
           "raw_dir": str(raw.relative_to(ROOT)), "machine": machine_info()}
    try:
        print(f"[1/5] sequential write, {a.size_gib} GiB, O_DIRECT, QD32", flush=True)
        drop_caches()
        w = fio(raw, "seq_write", big, a.size_gib, "write", extra=["--end_fsync=1"])
        print(f"[2/5] sequential read, {a.size_gib} GiB, O_DIRECT, QD32", flush=True)
        drop_caches()
        r = fio(raw, "seq_read", big, a.size_gib, "read")
        big.unlink()
        res["disk"] = {"file_gib": a.size_gib, "block": "1MiB", "iodepth": 32, "engine": "libaio",
                       "direct": True, "seq_write": w, "seq_read": r}

        print(f"[3/5] host cache test, {a.small_gib} GiB written then read back with O_DIRECT", flush=True)
        fio(raw, "small_write", small, a.small_gib, "write", extra=["--end_fsync=1"])
        drop_caches()
        s1 = fio(raw, "small_read_1", small, a.small_gib, "read")
        s2 = fio(raw, "small_read_2", small, a.small_gib, "read")
        small.unlink()
        ratio = max(s1["gbps"], s2["gbps"]) / r["gbps"]
        res["host_cache"] = {"file_gib": a.small_gib, "read_1": s1, "read_2": s2,
                             "ratio_vs_R": round(ratio, 2), "host_caches": ratio > 1.3}

        print("[4/5] warm page-cache read (buffered, 4 GiB already in RAM), 1 and 8 readers", flush=True)
        fio(raw, "warm_prep", warm, 4, "write", extra=["--end_fsync=1"])
        keep = ["--invalidate=0"]  # fio empties the file's page cache before a job unless told not to
        fio(raw, "warm_fill", warm, 4, "read", direct=False, extra=keep)  # pull it into the page cache
        w1 = fio(raw, "warm_read_1", warm, 4, "read", direct=False, extra=keep)
        w8 = fio(raw, "warm_read_8", warm, 4, "read", direct=False, numjobs=8, extra=keep)
        warm.unlink()
        res["warm_read"] = {"file_gib": 4, "readers_1": w1, "readers_8": w8}
    finally:
        for f in (big, small, warm):
            f.unlink(missing_ok=True)

    print("[5/5] memory read bandwidth, 1/2/4/8/16 threads", flush=True)
    res["memory"] = {str(n): membw(n) for n in (1, 2, 4, 8, 16)}

    res["ceilings"] = {
        "R_gbps": r["gbps"], "W_gbps": w["gbps"],
        "M1_gbps": res["memory"]["1"]["best_gbps"], "M16_gbps": res["memory"]["16"]["best_gbps"],
        "warm_read_8_gbps": w8["gbps"],
    }
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(res, indent=2) + "\n")
    print(json.dumps(res["ceilings"], indent=2))
    print(f"host cache: {'YES' if res['host_cache']['host_caches'] else 'no'} (ratio {ratio:.2f})")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
