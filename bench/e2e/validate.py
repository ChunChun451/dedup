#!/usr/bin/env python3
"""Check a bench/e2e/run.py result directory.

Always checks: session.json and plan.json exist; every cell file is well-formed; exit codes are 0; wall times are
positive; calibration was recorded; the virtual disk did not grow by more than 1 GB during a cell (SPEC 6.12f).
Options:
  --complete            every planned cell has a result
  --expect-tools A,B    these tools appear in the results
  --expect-verified     every (tool, config, dataset) with a restore cell has one verified restore (SPEC 6.9)
Exit 0 if all checks pass; otherwise prints each failure and exits 1.
"""
import argparse
import json
import pathlib
import sys

REQUIRED = ["id", "tool", "config", "dataset", "workload", "rep", "cache", "wall_s", "throughput_gbps", "exit_code",
            "input_bytes", "calibration", "vhdx_growth_gb", "user_s", "sys_s", "max_rss_kib"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--complete", action="store_true")
    ap.add_argument("--expect-tools", type=lambda s: s.split(","), default=[])
    ap.add_argument("--expect-verified", action="store_true")
    a = ap.parse_args()
    d = pathlib.Path(a.dir)
    errors = []

    for f in ("session.json", "plan.json"):
        if not (d / f).exists():
            errors.append(f"{f} missing")
    if errors:
        print("\n".join("FAIL " + e for e in errors))
        return 1
    plan = json.loads((d / "plan.json").read_text())
    cells = [json.loads(p.read_text()) for p in sorted((d / "cells").glob("*.json"))]
    if not cells:
        errors.append("no cell results")

    for c in cells:
        missing = [k for k in REQUIRED if k not in c]
        if missing:
            errors.append(f"{c.get('id', '?')}: missing fields {missing}")
            continue
        if c["exit_code"] != 0:
            errors.append(f"{c['id']}: exit code {c['exit_code']}")
        if not c["wall_s"] > 0:
            errors.append(f"{c['id']}: wall time {c['wall_s']}")
        if c["vhdx_growth_gb"] is not None and c["vhdx_growth_gb"] > 1.0:
            errors.append(f"{c['id']}: virtual disk grew {c['vhdx_growth_gb']} GB during the cell; re-run it")
        if c["workload"] == "backup" and "repo_bytes" not in c:
            errors.append(f"{c['id']}: repo_bytes missing")

    if a.complete:
        done = {(c["tool"], c["config"], c["dataset"], c["workload"], c["rep"]) for c in cells}
        for p in plan:
            if (p["tool"], p["config"], p["dataset"], p["workload"], p["rep"]) not in done:
                errors.append(f"planned cell without result: {p}")
    for t in a.expect_tools:
        if not any(c["tool"] == t for c in cells):
            errors.append(f"no results for tool {t}")
    if a.expect_verified:
        groups = {}
        for c in cells:
            if c["workload"] == "restore":
                key = (c["tool"], c["config"], c["dataset"])
                groups[key] = groups.get(key, False) or c.get("verified") is True
        if not groups:
            errors.append("no restore cells to verify")
        for key, ok in groups.items():
            if not ok:
                errors.append(f"restore never verified for {key}")

    for e in errors:
        print("FAIL", e)
    if errors:
        return 1
    print(f"OK {d}: {len(cells)} cells, {len(plan)} planned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
