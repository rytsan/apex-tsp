#!/usr/bin/env python3
"""Historical benchmark campaign (Sprint 6 M6.7): every instance of reference.csv (in_set = yes)
x {APEX exact, APEX scalable, Concorde, LKH-3}, one run at a time, resumable.

  python3 engine/tools/history/run_history.py [--out out/history/final] [--only a280,pr1002]
                                              [--solvers apex_exact,apex_scalable,concorde,lkh] [--dry-run]

Per run: <out>/<instance>__<solver>.json (+ .log, APEX .tour). A run whose JSON exists is
skipped, so an interrupted campaign continues where it stopped. Time boxes (TIME_BOX) depend on n
and are the same for APEX exact and Concorde; APEX scalable and LKH-3 share theirs. Concorde and
LKH-3 are test-environment baselines (engine/tools/baselines), never part of the product.
"""
import argparse
import csv
import datetime
import json
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
APEX = os.path.join(ROOT, "build", "benchmark", "apex_tsp")
BASE = os.path.join(ROOT, "engine", "tools", "baselines", "run_baseline.py")
THREADS = 5
MEM_MB = 4096
SEED = 1
EXACT_MAX_N = 4000  # exact runs (APEX and Concorde) up to this size; beyond: scalable / LKH only


def time_box(n, kind):
    """Seconds per run. kind: 'exact' (APEX exact, Concorde) or 'heur' (APEX scalable, LKH-3)."""
    if kind == "exact":
        return 60 if n < 200 else 600 if n < 1000 else 1800 if n < 3000 else 3600
    return 30 if n < 1000 else 120 if n < 5000 else 600


def plan(rows, solvers):
    for r in rows:
        n = int(r["n"])
        for s in solvers:
            if s in ("apex_exact", "concorde") and n > EXACT_MAX_N:
                continue
            yield r["instance"], n, s, time_box(n, "exact" if s in ("apex_exact", "concorde") else "heur")


def run_apex(inst, mode, limit, out_base):
    report, tour, log = out_base + ".report.json", out_base + ".tour", out_base + ".log"
    cmd = [APEX, "--instance", inst, "--mode", mode, "--time-limit", str(limit), "--threads", str(THREADS),
           "--mem-limit-mb", str(MEM_MB), "--seed", str(SEED), "--report", report, "--out", tour]
    with open(log, "w") as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
    r = json.load(open(report)) if os.path.exists(report) else {"status": f"EXIT_{rc}"}
    os.path.exists(report) and os.remove(report)
    return {"solver": "apex_" + mode, "status": r.get("status"), "ub": r.get("ub"), "lb": r.get("lb"),
            "lb_source": r.get("lb_source"), "wall_s": r.get("wall_s"), "ram_peak_bytes": r.get("ram_peak_bytes"),
            "threads": THREADS, "seed": SEED, "time_limit_s": limit, "exit_code": rc,
            "iterations": r.get("iterations"), "cuts_by_family": r.get("cuts_by_family")}


def run_baseline(solver, inst, limit, out_base):
    report = out_base + ".json.tmp"
    with open(out_base + ".log", "w") as f:
        rc = subprocess.call([sys.executable, BASE, solver, inst, "--time-limit", str(limit), "--seed", str(SEED),
                              "--report", report], stdout=f, stderr=subprocess.STDOUT)
    r = json.load(open(report)) if os.path.exists(report) else {"status": f"EXIT_{rc}"}
    os.path.exists(report) and os.remove(report)
    r["exit_code"] = rc
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "history", "final"))
    ap.add_argument("--only", default="")
    ap.add_argument("--solvers", default="apex_exact,apex_scalable,concorde,lkh")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    rows = [r for r in csv.DictReader(open(os.path.join(HERE, "reference.csv"))) if r["in_set"] == "yes"]
    if a.only:
        keep = set(a.only.split(","))
        rows = [r for r in rows if r["instance"] in keep]
    rows.sort(key=lambda r: int(r["n"]))
    todo = list(plan(rows, a.solvers.split(",")))
    total = sum(t for *_, t in todo)
    print(f"{len(todo)} runs, worst case {total / 3600:.1f} h (time boxes; most runs stop earlier)")
    if a.dry_run:
        for inst, n, s, t in todo:
            print(f"  {inst:<14} n={n:<6} {s:<14} {t} s")
        return
    os.makedirs(a.out, exist_ok=True)
    meta = os.path.join(a.out, "meta.json")
    if not os.path.exists(meta):
        cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), platform.processor())
        commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
        versions = open(os.path.join(ROOT, "third_party_test", "VERSIONS.txt")).read() if os.path.exists(
            os.path.join(ROOT, "third_party_test", "VERSIONS.txt")) else ""
        json.dump({"commit": commit, "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                   "cpu": cpu, "logical_cpus": os.cpu_count(), "os": platform.platform(), "threads_apex": THREADS,
                   "mem_limit_mb": MEM_MB, "seed": SEED, "baselines": versions, "exact_max_n": EXACT_MAX_N},
                  open(meta, "w"), indent=1)
    done = 0
    for inst_name, n, solver, limit in todo:
        base = os.path.join(a.out, f"{inst_name}__{solver}")
        if os.path.exists(base + ".json"):
            done += 1
            continue
        inst = os.path.join(ROOT, "data", "tsplib", inst_name + ".tsp")
        t0 = time.time()
        print(f"[{datetime.datetime.now():%H:%M:%S}] {inst_name} n={n} {solver} limit={limit}s", flush=True)
        r = run_apex(inst, solver.split("_")[1], limit, base) if solver.startswith("apex") else run_baseline(solver, inst, limit, base)
        r.update(instance=inst_name, n=n, solver=solver)
        with open(base + ".json.tmp", "w") as f:
            json.dump(r, f)
        os.replace(base + ".json.tmp", base + ".json")
        done += 1
        print(f"  -> {r.get('status')} ub={r.get('ub')} lb={r.get('lb')} wall={r.get('wall_s')} ({time.time() - t0:.0f} s) [{done}/{len(todo)}]", flush=True)


if __name__ == "__main__":
    main()
