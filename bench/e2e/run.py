#!/usr/bin/env python3
"""End-to-end benchmark harness (roadmap W02.3; method in SPEC.md section 6).

Runs backup tools described by bench/tools/<tool>.toml on datasets from data/, and writes one JSON file per cell
(tool x config x dataset x workload x repetition) plus summary.json into --out.

Per cell it records: wall time from start to exit plus a following `sync`; CPU time, peak RSS and filesystem bytes
from /usr/bin/time -v; repo size; a CPU calibration score taken just before (SPEC 6.8); growth of the WSL virtual
disk; Windows' per-second disk counters and the drive-state label fast/slow/n/a (SPEC 6.12); and, once per
(tool, config, dataset), whether the restored tree is identical to the source (SPEC 6.9).

Tool order inside each (dataset, workload, config) block rotates with the repetition number: a cyclic Latin square.
*-small datasets run warm (one unmeasured run first), full datasets run cold (page cache dropped), unless --cache says
otherwise.

Example:  python3 bench/e2e/run.py --tools cp --datasets d3-small --reps 3 --out results/smoke
"""
import argparse
import datetime
import json
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import time
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bench/hw"))
sys.path.insert(0, str(ROOT / "scripts/datasets"))
from diskstate import WinDiskCounters, classify_drive_state, vhdx_bytes  # noqa: E402
from machine import machine_info  # noqa: E402
from treehash import tree_digest  # noqa: E402

DATA = ROOT / "data"
SCRATCH = DATA / "scratch/e2e"
CALIB = ROOT / "build/release/bench/hw/calib"
CALIB_TOLERANCE = 0.05
COOLDOWN_S = 60
MAX_COOLDOWNS = 3
WORKLOADS = ("backup", "restore")


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


def load_tool(name):
    path = ROOT / "bench/tools" / f"{name}.toml"
    with open(path, "rb") as f:
        t = tomllib.load(f)
    t["file"] = str(path.relative_to(ROOT))
    return t


def expand(cmd, subst, config_args):
    out = []
    for part in cmd:
        if part == "{config_args}":
            out.extend(config_args)
        else:
            out.append(part.format(**subst))
    return out


def dataset_bytes(path):
    if path.is_file():
        return path.stat().st_size
    return sum((pathlib.Path(d) / f).lstat().st_size for d, _, files in os.walk(path) for f in files)


def drop_caches():
    subprocess.run(["sudo", "-n", "/usr/local/sbin/dedup-drop-caches"], check=True)


def calibrate():
    return json.loads(subprocess.run([str(CALIB)], check=True, text=True, capture_output=True).stdout)["score"]


def parse_time_v(path):
    """Fields from GNU time -v output that we keep."""
    want = {"User time (seconds)": ("user_s", float), "System time (seconds)": ("sys_s", float),
            "Maximum resident set size (kbytes)": ("max_rss_kib", int),
            "File system inputs": ("fs_read_bytes", lambda v: int(v) * 512),
            "File system outputs": ("fs_write_bytes", lambda v: int(v) * 512),
            "Exit status": ("exit_status", int)}
    res = {}
    for line in pathlib.Path(path).read_text().splitlines():
        key, _, val = line.strip().rpartition(": ")
        if key in want:
            name, conv = want[key]
            res[name] = conv(val)
    return res


def du_bytes(path):
    if not path.exists():
        return 0
    return int(subprocess.run(["du", "-sb", str(path)], check=True, text=True, capture_output=True).stdout.split()[0])


def run_cmd(cmd, env, cwd):
    p = subprocess.run(cmd, env=env, cwd=cwd, text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f"command failed ({p.returncode}): {' '.join(cmd)}\n{p.stderr[-2000:]}")


class Session:
    def __init__(self, a):
        self.a = a
        self.out = pathlib.Path(a.out)
        self.cells_dir = self.out / "cells"
        self.tools = {n: load_tool(n) for n in a.tools}
        self.secrets = SCRATCH / "secrets"
        self.src_digest = {}
        self.verified = set()
        self.calib_baseline = None

    def env_for(self, tool):
        env = dict(os.environ)
        for k, v in tool.get("env", {}).items():
            env[k] = v.format(**self.subst_base(tool))
        return env

    def subst_base(self, tool):
        return {"bin": str(ROOT / "tools/bin"), "secrets": str(self.secrets)}

    def plan(self):
        cells = []
        for ds in self.a.datasets:
            for wl in self.a.workloads:
                for rep in range(self.a.reps):
                    names = list(self.a.tools)
                    order = names[rep % len(names):] + names[:rep % len(names)]
                    for pos, tool in enumerate(order):
                        for cfg in self.configs(tool):
                            cells.append({"dataset": ds, "workload": wl, "config": cfg, "rep": rep, "tool": tool,
                                          "order_index": pos})
        return cells

    def configs(self, tool):
        have = list(self.tools[tool].get("configs", {"defaults": {}}).keys())
        return [c for c in have if not self.a.configs or c in self.a.configs]

    def cache_mode(self, ds):
        if self.a.cache != "auto":
            return self.a.cache
        return "warm" if ds.endswith("-small") else "cold"

    # ---------- one cell ----------
    def run_cell(self, c):
        tool = self.tools[c["tool"]]
        cfg_args = tool.get("configs", {}).get(c["config"], {}).get("args", [])
        src = DATA / c["dataset"]
        work = SCRATCH / f"{c['tool']}-{c['config']}-{c['dataset']}"
        repo, target, ok_marker = work / "repo", work / "restore", work / "repo.ok"
        subst = {**self.subst_base(tool), "src": str(src), "repo": str(repo), "target": str(target)}
        env = self.env_for(tool)
        cmds = tool["commands"]

        def fresh_repo():
            shutil.rmtree(repo, ignore_errors=True)
            ok_marker.unlink(missing_ok=True)
            repo.mkdir(parents=True)
            if cmds.get("init"):
                run_cmd(expand(cmds["init"], subst, cfg_args), env, work)

        def fresh_target():
            shutil.rmtree(target, ignore_errors=True)
            target.mkdir(parents=True)

        work.mkdir(parents=True, exist_ok=True)
        cache = self.cache_mode(c["dataset"])
        if c["workload"] == "backup":
            cmd = expand(cmds["backup"], subst, cfg_args)
            fresh_repo()
            if cache == "warm":
                run_cmd(cmd, env, work)  # unmeasured warm-up run; reads the dataset into the page cache
                fresh_repo()
        else:
            cmd = expand(cmds["restore"], subst, cfg_args)
            if not ok_marker.exists():  # a repo with one snapshot of this dataset (made untimed)
                fresh_repo()
                run_cmd(expand(cmds["backup"], subst, cfg_args), env, work)
                ok_marker.touch()
            fresh_target()
            if cache == "warm":
                run_cmd(cmd, env, work)
                fresh_target()
        if cache == "cold":
            drop_caches()

        calib = self.check_calibration()
        tfile = work / "time.txt"
        v0 = vhdx_bytes()
        counters = WinDiskCounters(f"{os.getpid()}-{len(os.listdir(self.cells_dir))}").start() \
            if self.a.win_counters else None
        started = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        t0w, t0 = time.time(), time.monotonic()
        p = subprocess.run(["/usr/bin/time", "-v", "-o", str(tfile), *cmd], env=env, cwd=work, text=True,
                           capture_output=True)
        subprocess.run(["sync"], check=True)
        wall = time.monotonic() - t0
        t1w = time.time()
        samples = counters.stop() if counters else []
        v1 = vhdx_bytes()

        rec = {"schema": 1, **c, "cache": cache, "started": started, "cmd": cmd,
               "input_bytes": self.input_bytes[c["dataset"]], "wall_s": round(wall, 4),
               "throughput_gbps": round(self.input_bytes[c["dataset"]] / wall / 1e9, 4),
               "exit_code": p.returncode, "stderr_tail": p.stderr[-2000:] if p.returncode else "",
               **parse_time_v(tfile), "calibration": calib,
               "vhdx_growth_gb": None if v0 is None or v1 is None else round((v1 - v0) / 1e9, 3),
               "drive": classify_drive_state(samples, t0w, t1w) if counters else None,
               "win_samples": [[round(s[0] - t0w, 2), round(s[1] / 1e6, 1), round(s[2], 2), round(s[3])]
                               for s in samples]}
        if c["workload"] == "backup":
            rec["repo_bytes"] = du_bytes(repo)
            if p.returncode == 0:
                ok_marker.touch()  # this fresh repo now holds one snapshot; restore cells can reuse it
        else:
            key = (c["tool"], c["config"], c["dataset"])
            rec["verified"] = None
            if p.returncode == 0 and key not in self.verified:
                if c["dataset"] not in self.src_digest:
                    self.src_digest[c["dataset"]] = tree_digest(str(src))
                rec["verified"] = tree_digest(str(target)) == self.src_digest[c["dataset"]]
                if rec["verified"]:
                    self.verified.add(key)
            shutil.rmtree(target, ignore_errors=True)
        return rec

    def check_calibration(self):
        score = calibrate()
        res = {"score": score, "baseline": self.calib_baseline, "cooldowns": 0}
        while abs(score / self.calib_baseline - 1) > CALIB_TOLERANCE and res["cooldowns"] < MAX_COOLDOWNS:
            log(f"  calibration {score:.3f} is >5% off baseline {self.calib_baseline:.3f}; cooling down {COOLDOWN_S} s")
            time.sleep(COOLDOWN_S)
            res["cooldowns"] += 1
            score = calibrate()
        res["score"] = score
        res["deviation"] = round(score / self.calib_baseline - 1, 4)
        return res

    # ---------- session ----------
    def run(self):
        plan = self.plan()
        if self.a.dry_run:
            seen = set()
            for c in plan:
                key = (c["tool"], c["config"], c["dataset"], c["workload"])
                if key in seen:
                    continue
                seen.add(key)
                tool = self.tools[c["tool"]]
                subst = {**self.subst_base(tool), "src": str(DATA / c["dataset"]), "repo": "{repo}",
                         "target": "{target}"}
                cmd = expand(tool["commands"][c["workload"]], subst,
                             tool.get("configs", {}).get(c["config"], {}).get("args", []))
                print(f"CMD {c['tool']} {c['config']} {c['dataset']} {c['workload']}: {' '.join(cmd)}")
            return 0

        for ds in self.a.datasets:
            if not (DATA / ds).exists():
                sys.exit(f"dataset {ds} not found; run scripts/datasets/make.sh first")
        if not CALIB.exists():
            sys.exit("build first: cmake --build --preset release")
        self.cells_dir.mkdir(parents=True, exist_ok=True)
        self.secrets.mkdir(parents=True, exist_ok=True)
        (self.secrets / "password").write_text("dedup-bench\n")
        self.input_bytes = {ds: dataset_bytes(DATA / ds) for ds in self.a.datasets}
        self.calib_baseline = statistics.median(calibrate() for _ in range(3))
        session = {"schema": 1, "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                   "git_sha": subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True,
                                             capture_output=True).stdout.strip(),
                   "args": vars(self.a), "calibration_baseline": self.calib_baseline,
                   "machine": machine_info([(f"version_{n}", t["version_cmd"]) for n, t in self.tools.items()]),
                   "tools": {n: t["file"] for n, t in self.tools.items()}, "input_bytes": self.input_bytes,
                   "planned_cells": len(plan)}
        (self.out / "session.json").write_text(json.dumps(session, indent=2) + "\n")
        (self.out / "plan.json").write_text(json.dumps(plan, indent=2) + "\n")

        records = []
        for i, c in enumerate(plan):
            cid = f"{c['dataset']}.{c['workload']}.{c['config']}.{c['tool']}.r{c['rep']}"
            log(f"cell {i + 1}/{len(plan)} {cid}")
            rec = {"id": cid, **self.run_cell(c)}
            (self.cells_dir / f"{cid}.json").write_text(json.dumps(rec, indent=2) + "\n")
            drive = rec["drive"]["state"] if rec["drive"] else "-"
            log(f"  {rec['wall_s']:.2f} s, {rec['throughput_gbps']:.2f} GB/s, exit {rec['exit_code']}, drive {drive}"
                + (f", verified {rec['verified']}" if rec.get("verified") is not None else ""))
            records.append(rec)
        if not self.a.keep:
            shutil.rmtree(SCRATCH, ignore_errors=True)
        (self.out / "summary.json").write_text(json.dumps(summarize(records), indent=2) + "\n")
        log(f"wrote {len(records)} cells to {self.out}")
        return 0


def summarize(records):
    groups = {}
    for r in records:
        groups.setdefault((r["tool"], r["config"], r["dataset"], r["workload"], r["cache"]), []).append(r)
    out = []
    for (tool, cfg, ds, wl, cache), rs in sorted(groups.items()):
        walls = [r["wall_s"] for r in rs]
        mean = statistics.mean(walls)
        cov = statistics.stdev(walls) / mean if len(walls) > 1 else 0.0
        states = {}
        for r in rs:
            s = r["drive"]["state"] if r.get("drive") else "unmeasured"
            states[s] = states.get(s, 0) + 1
        out.append({"tool": tool, "config": cfg, "dataset": ds, "workload": wl, "cache": cache, "reps": len(rs),
                    "wall_median_s": round(statistics.median(walls), 4), "wall_min_s": min(walls),
                    "wall_max_s": max(walls), "cov": round(cov, 4), "cov_flag": cov > 0.05,
                    "throughput_median_gbps": round(statistics.median(r["throughput_gbps"] for r in rs), 4),
                    "repo_bytes_median": statistics.median(r["repo_bytes"] for r in rs) if wl == "backup" else None,
                    "drive_states": states, "verified": any(r.get("verified") for r in rs) if wl == "restore" else None,
                    "failures": sum(r["exit_code"] != 0 for r in rs)})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tools", required=True, type=lambda s: s.split(","))
    ap.add_argument("--datasets", required=True, type=lambda s: s.split(","))
    ap.add_argument("--workloads", default=list(WORKLOADS), type=lambda s: s.split(","))
    ap.add_argument("--configs", default=[], type=lambda s: s.split(","), help="default: every config in each TOML")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--cache", choices=["auto", "cold", "warm"], default="auto")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-win-counters", dest="win_counters", action="store_false")
    ap.add_argument("--keep", action="store_true", help="keep repos and restore dirs in data/scratch/e2e")
    ap.add_argument("--dry-run", action="store_true", help="print one CMD line per tool/config/dataset/workload")
    a = ap.parse_args()
    for w in a.workloads:
        if w not in WORKLOADS:
            sys.exit(f"unknown workload {w}; known: {', '.join(WORKLOADS)}")
    return Session(a).run()


if __name__ == "__main__":
    sys.exit(main())
