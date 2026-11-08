#!/usr/bin/env python3
"""Offline check of run_baseline.py: instance reader + tour length agree with known optima."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_baseline as rb  # noqa: E402

ROOT = rb.ROOT
for name, opt in (("a280", 2579), ("pr1002", 259045), ("berlin52", 7542), ("eil51", 426)):
    _, xs, ys, ceil = rb.read_instance(os.path.join(ROOT, "data", name + ".tsp"))
    tour, sec = [], False
    for line in open(os.path.join(ROOT, "data", name + ".opt.tour")):
        s = line.strip()
        if s == "TOUR_SECTION":
            sec = True
        elif sec and s not in ("-1", "EOF"):
            tour.extend(int(v) - 1 for v in s.split())
        elif sec:
            break
    assert rb.tour_length(tour, xs, ys, ceil) == opt, name
_, xs, ys, ceil = rb.read_instance(os.path.join(ROOT, "data", "pla85900.tsp"))
assert ceil and len(xs) == 85900
print("run_baseline OK: tour lengths match 4 TSPLIB optima, CEIL_2D detected")
