#!/usr/bin/env python3
"""Check that a hardware baseline file (bench/hw/run.py output) is complete and plausible.

Usage: python3 bench/hw/validate.py results/hw/baseline.json
Exit 0 if every check passes; otherwise prints each failure and exits 1.
"""
import json
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: validate.py <baseline.json>")
    b = json.load(open(sys.argv[1]))
    errors = []

    def need(cond, msg):
        if not cond:
            errors.append(msg)

    need(b.get("schema") == 1, "schema must be 1")
    c = b.get("ceilings", {})
    for key, lo, hi in [("R_gbps", 0.2, 15), ("W_gbps", 0.2, 15), ("M1_gbps", 3, 150),
                        ("M16_gbps", 3, 150), ("warm_read_8_gbps", 0.5, 150)]:
        v = c.get(key)
        need(isinstance(v, (int, float)) and lo <= v <= hi, f"ceilings.{key}={v} not in [{lo}, {hi}]")
    need(c.get("M16_gbps", 0) >= 0.9 * c.get("M1_gbps", 1), "M16 should not be much lower than M1")

    d = b.get("disk", {})
    need(d.get("file_gib", 0) >= 32, "disk test file must be >= 32 GiB (bigger than host RAM)")
    need(d.get("direct") is True, "disk test must use O_DIRECT")
    for side in ("seq_write", "seq_read"):
        j = d.get(side, {})
        need(j.get("error") == 0, f"disk.{side} reported an fio error")
        need(j.get("io_bytes", 0) >= d.get("file_gib", 0) << 30, f"disk.{side} moved fewer bytes than the file size")

    h = b.get("host_cache", {})
    need(isinstance(h.get("host_caches"), bool), "host_cache.host_caches must be true/false")
    need(isinstance(h.get("ratio_vs_R"), (int, float)), "host_cache.ratio_vs_R missing")

    for n in ("1", "2", "4", "8", "16"):
        need(n in b.get("memory", {}), f"memory result for {n} threads missing")

    m = b.get("machine", {})
    need(m.get("nproc") == 16, f"expected 16 CPUs, got {m.get('nproc')}")
    need(m.get("power_line") == "Online", f"laptop must be on AC power (got {m.get('power_line')})")

    for e in errors:
        print("FAIL", e)
    if errors:
        sys.exit(1)
    print(f"OK {sys.argv[1]}: R={c['R_gbps']} W={c['W_gbps']} M1={c['M1_gbps']} M16={c['M16_gbps']} GB/s")


if __name__ == "__main__":
    main()
