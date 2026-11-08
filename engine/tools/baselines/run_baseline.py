#!/usr/bin/env python3
"""Runs a same-machine baseline (Sprint 6 M6.6, test environment only) on one instance and writes
the per-run JSON of run_benchmarks.sh / run_history.sh, so the reports can set it beside APEX.

  python3 engine/tools/baselines/run_baseline.py concorde data/tsplib/a280.tsp --time-limit 600 --report r.json
  python3 engine/tools/baselines/run_baseline.py lkh      data/tsplib/a280.tsp --time-limit 600 --report r.json

The tour each solver writes is re-read and its length recomputed here with the TSPLIB EUC_2D /
CEIL_2D rules (the solver's own number is kept only as a cross-check). Concorde: single thread,
`concorde -s <seed>`; OPTIMAL_PROVEN when it finishes ("Optimal Solution"), else TIME_LIMIT with
its best tour. LKH: heuristic (no proof); LB = its Held-Karp ascent bound ("Lower bound").
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
BIN = {
    "concorde": os.path.join(ROOT, "third_party_test", "concorde", "TSP", "concorde"),
    "lkh": os.path.join(ROOT, "third_party_test", "lkh", "LKH"),
}


def read_instance(path):
    xs, ys, ceil, name = [], [], False, os.path.splitext(os.path.basename(path))[0]
    coords = False
    for line in open(path):
        s = line.strip()
        if not coords:
            if s.startswith("EDGE_WEIGHT_TYPE"):
                ceil = "CEIL_2D" in s
            if s.startswith("NAME"):
                name = s.split(":", 1)[1].strip()
            coords = s.startswith("NODE_COORD_SECTION")
            continue
        if s == "EOF" or not s:
            break
        p = s.split()
        xs.append(float(p[1]))
        ys.append(float(p[2]))
    return name, xs, ys, ceil


def tour_length(tour, xs, ys, ceil):
    total = 0
    for i, a in enumerate(tour):
        b = tour[(i + 1) % len(tour)]
        d = math.sqrt((xs[a] - xs[b]) ** 2 + (ys[a] - ys[b]) ** 2)
        total += math.ceil(d) if ceil else int(math.floor(d + 0.5))
    return total


def run(cmd, cwd, limit):
    """-> (returncode | None on timeout, stdout text, wall s, peak RSS bytes)."""
    t0 = time.monotonic()
    with open(os.path.join(cwd, "solver.log"), "w") as log:
        p = subprocess.Popen(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        timed_out = False
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid:
                break
            if limit and time.monotonic() - t0 > limit:
                p.kill()
                pid, status, ru = os.wait4(p.pid, 0)
                timed_out = True
                break
            time.sleep(0.05)
    wall = time.monotonic() - t0
    out = open(os.path.join(cwd, "solver.log"), errors="replace").read()
    return (None if timed_out else os.waitstatus_to_exitcode(status)), out, wall, ru.ru_maxrss * 1024


def concorde(inst, limit, seed, work):
    shutil.copy(inst, os.path.join(work, "in.tsp"))
    rc, out, wall, rss = run([BIN["concorde"], "-s", str(seed), "-o", "out.sol", "in.tsp"], work, limit)
    tour = []
    sol = os.path.join(work, "out.sol")
    if os.path.exists(sol):  # first number = n, then the node order (0-based)
        nums = open(sol).read().split()
        tour = [int(v) for v in nums[1:]]
    m = re.search(r"Optimal Solution: ([\d.]+)", out)
    proven = rc == 0 and m is not None
    return {"status": "OPTIMAL_PROVEN" if proven else ("TIME_LIMIT" if rc is None else f"EXIT_{rc}"),
            "solver_ub": float(m.group(1)) if m else None, "lb_source": "lp" if proven else None,
            "wall_s": wall, "ram_peak_bytes": rss, "tour": tour, "log": out}


def lkh(inst, limit, seed, work, n=0):
    shutil.copy(inst, os.path.join(work, "in.tsp"))
    par = os.path.join(work, "run.par")
    with open(par, "w") as f:
        f.write(f"PROBLEM_FILE = in.tsp\nOUTPUT_TOUR_FILE = out.tour\nRUNS = 1\nSEED = {seed}\n")
        if limit:
            f.write(f"TIME_LIMIT = {limit}\n")
        if n > 20000:  # the default alpha-nearness ascent is quadratic in time and memory at this size
            f.write("CANDIDATE_SET_TYPE = POPMUSIC\nINITIAL_PERIOD = 100\n")
    rc, out, wall, rss = run([BIN["lkh"], "run.par"], work, limit * 1.5 + 60 if limit else 0)
    tour = []
    tf = os.path.join(work, "out.tour")
    if os.path.exists(tf):
        sec = False
        for line in open(tf):
            s = line.strip()
            if s == "TOUR_SECTION":
                sec = True
            elif sec:
                if s in ("-1", "EOF"):
                    break
                tour.append(int(s) - 1)
    m = re.search(r"Cost\.min = (\d+)", out)
    lb = re.search(r"Lower bound = ([\d.]+)", out)
    return {"status": "HEURISTIC" if rc == 0 else ("TIME_LIMIT" if rc is None else f"EXIT_{rc}"),
            "solver_ub": float(m.group(1)) if m else None,
            "lb": math.ceil(float(lb.group(1)) - 1e-6) if lb else None, "lb_source": "lagrangian" if lb else None,
            "wall_s": wall, "ram_peak_bytes": rss, "tour": tour, "log": out}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("solver", choices=sorted(BIN))
    ap.add_argument("instance")
    ap.add_argument("--time-limit", type=float, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--report", required=True)
    ap.add_argument("--keep", help="directory for the solver's files (default: temporary)")
    a = ap.parse_args()
    if not os.access(BIN[a.solver], os.X_OK):
        sys.exit(f"{BIN[a.solver]} not built: run engine/tools/baselines/build_baselines.sh")
    name, xs, ys, ceil = read_instance(a.instance)
    work = a.keep or tempfile.mkdtemp(prefix=f"apex_{a.solver}_")
    os.makedirs(work, exist_ok=True)
    inst = os.path.abspath(a.instance)
    r = concorde(inst, a.time_limit, a.seed, work) if a.solver == "concorde" else lkh(inst, a.time_limit, a.seed, work, len(xs))
    tour = r.pop("tour")
    log = r.pop("log")
    valid = len(tour) == len(xs) and sorted(tour) == list(range(len(xs)))
    ub = tour_length(tour, xs, ys, ceil) if valid else None
    if r["status"] == "OPTIMAL_PROVEN":
        r["lb"] = ub
    rep = {"instance": name, "solver": a.solver, "status": r["status"], "ub": ub,
           "lb": r.get("lb"), "lb_source": r.get("lb_source"), "wall_s": round(r["wall_s"], 3),
           "ram_peak_bytes": r["ram_peak_bytes"], "threads": 1, "seed": a.seed, "time_limit_s": a.time_limit,
           "tour_valid": valid, "solver_reported_ub": r["solver_ub"],
           "ub_matches_solver": ub is not None and r["solver_ub"] is not None and abs(ub - r["solver_ub"]) < 0.5}
    with open(a.report, "w") as f:
        json.dump(rep, f)
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    print(json.dumps(rep))
    if not valid:
        print(log[-2000:], file=sys.stderr)
    return 0 if valid else 3


if __name__ == "__main__":
    sys.exit(main())
