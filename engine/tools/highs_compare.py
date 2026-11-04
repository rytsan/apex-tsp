#!/usr/bin/env python3
"""Run HiGHS (test environment only, never linked into APEX-TSP) on the same MPS files
used by lp_bench. Reports wall time and simplex iterations for two configurations:
  simplex_nopresolve : dual simplex, presolve off  (same problem the native engine solves)
  default            : HiGHS defaults (presolve on, automatic solver choice)
Usage: highs_compare.py <file.mps> [...]
"""
import sys
import time

import highspy


def run(path, presolve, solver):
    h = highspy.Highs()
    h.setOptionValue("output_flag", False)
    h.setOptionValue("threads", 1)
    h.setOptionValue("presolve", presolve)
    h.setOptionValue("solver", solver)
    if solver == "simplex":
        h.setOptionValue("simplex_strategy", 1)  # dual simplex
    h.readModel(path)
    t0 = time.perf_counter()
    h.run()
    dt = time.perf_counter() - t0
    info = h.getInfo()
    return h.modelStatusToString(h.getModelStatus()), info.objective_function_value, info.simplex_iteration_count, dt


print(f"highspy {highspy.Highs().version()}")
print(f"{'file':<22} {'config':<20} {'status':<10} {'objective':>22} {'iters':>8} {'time_s':>9}")
for path in sys.argv[1:]:
    for name, presolve, solver in (("simplex_nopresolve", "off", "simplex"), ("default", "on", "choose")):
        st, obj, it, dt = run(path, presolve, solver)
        print(f"{path.split('/')[-1]:<22} {name:<20} {st:<10} {obj:22.10e} {it:8d} {dt:9.3f}")
