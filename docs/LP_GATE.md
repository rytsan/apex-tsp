# LP Gate (Sprint 1, Band E1) — native dual simplex vs. HiGHS

HiGHS is used **only in the test environment** for this comparison. It is installed in a throwaway
Python virtualenv (`pip install highspy`, version 1.15.1) and is never linked into APEX-TSP.

## Hardware / build

- CPU: AMD Ryzen 5 3600X (6 cores; 5 physical cores visible under WSL2), 25 GB RAM
- OS: Linux 6.18.54.1-microsoft-standard-WSL2; GCC 13.3.0; preset `benchmark`
  (`-O3 -DNDEBUG -ffp-contract=off -march=x86-64-v3`)
- Single thread on both sides; seed 1; no presolve in the native engine.

## Procedure

```
bash engine/tools/fetch_instances.sh                      # TSPLIB + Netlib MPS (COIN-OR Data-Netlib copies)
build/benchmark/lp_bench --pricing devex --write-mps out/stress_pr1002.mps
build/benchmark/lp_bench --pricing dse
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install highspy
/tmp/venv/bin/python engine/tools/highs_compare.py data/netlib/{adlittle,25fv47,sc205,share2b,grow22}.mps out/stress_pr1002.mps
```

HiGHS runs in two configurations:
- `simplex_nopresolve`: dual simplex with presolve off. This is the same problem the native engine
  solves, so the gate is evaluated on it.
- `default`: HiGHS defaults, shown for context.

Times are solver wall time only. File reading is excluded on both sides.

Reference optima: https://www.netlib.org/lp/data/readme (PROBLEM SUMMARY TABLE).

## Native engine (`lp_bench`, Devex = default pricing)

```
lp               rows   cols  status                       objective              reference    rel_err    iters refact    flips    time_s    iters/s   rss_MB
ADLITTLE           56     97  optimal               2.2549496316e+05       2.2549496316e+05   1.06e-11       84      8       17     0.001      80822      3.8
25FV47            821   1571  optimal               5.5018458883e+03       5.5018458883e+03   2.87e-12     4064    154       27     0.608       6688      5.0
SC205             205    203  optimal              -5.2202061212e+01      -5.2202061212e+01   5.61e-12      208      8        0     0.003      67215      5.0
SHARE2B            96     79  optimal              -4.1573224074e+02      -4.1573224074e+02   3.41e-12      135      7        6     0.002      79231      5.0
GROW22            440    946  optimal              -1.6083433648e+08      -1.6083433648e+08   1.59e-11     3449    112     4606     0.390       8848      5.3
netlib_total time_s=1.003 iterations=7940 objective_failures=0
stress_pr1002    6002   5263  optimal               2.4509341667e+05                      -          -     2271     25     1890     0.770       2951     20.7
stress nnz=340093 used_artificial_bounds=0
```

## Native engine (`lp_bench --pricing dse`)

```
ADLITTLE           56     97  optimal               2.2549496316e+05       2.2549496316e+05   1.06e-11       81      7       14     0.001      93716      3.8
25FV47            821   1571  optimal               5.5018458883e+03       5.5018458883e+03   2.35e-12     2549     91       17     0.366       6958      5.0
SC205             205    203  optimal              -5.2202061212e+01      -5.2202061212e+01   5.61e-12      208      5        0     0.002      92146      5.0
SHARE2B            96     79  optimal              -4.1573224074e+02      -4.1573224074e+02   3.41e-12      136      8        5     0.002      63851      5.0
GROW22            440    946  optimal              -1.6083433648e+08      -1.6083433648e+08   1.59e-11     3138     99     4966     0.375       8358      5.2
netlib_total time_s=0.747 iterations=6112 objective_failures=0
stress_pr1002    6002   5263  optimal               2.4509341667e+05                      -          -     1622     17     1252     0.491       3305     20.0
```

(The `rows` column counts constraint rows only; the Netlib readme also counts the objective row.)

## HiGHS 1.15.1 (`highs_compare.py`)

```
file                   config               status                  objective    iters    time_s
adlittle.mps           simplex_nopresolve   Optimal          2.2549496316e+05       74     0.001
adlittle.mps           default              Optimal          2.2549496316e+05       87     0.001
25fv47.mps             simplex_nopresolve   Optimal          5.5018458883e+03     3149     0.174
25fv47.mps             default              Optimal          5.5018458883e+03     2583     0.135
sc205.mps              simplex_nopresolve   Optimal         -5.2202061212e+01      237     0.002
sc205.mps              default              Optimal         -5.2202061212e+01       84     0.002
share2b.mps            simplex_nopresolve   Optimal         -4.1573224074e+02      104     0.001
share2b.mps            default              Optimal         -4.1573224074e+02       99     0.002
grow22.mps             simplex_nopresolve   Optimal         -1.6083433648e+08     1693     0.054
grow22.mps             default              Optimal         -1.6083433648e+08     1934     0.074
stress_pr1002.mps      simplex_nopresolve   Optimal          2.4509341667e+05     1844     0.110
stress_pr1002.mps      default              Optimal          2.4509341667e+05     1882     0.290
```

## G1.2 evaluation (target: total time <= 10x, iterations <= 3x)

Totals over the 5 Netlib LPs:

| Native pricing | vs. HiGHS config | time ratio | iteration ratio | worst single-LP iteration ratio |
|---|---|---|---|---|
| Devex (default) | simplex_nopresolve (0.232 s, 5257 it) | 1.003 / 0.232 = **4.3x** | 7940 / 5257 = **1.51x** | GROW22 3449 / 1693 = 2.04x |
| Devex (default) | default (0.214 s, 4787 it) | 4.7x | 1.66x | — |
| DSE | simplex_nopresolve | 0.747 / 0.232 = **3.2x** | 6112 / 5257 = **1.16x** | GROW22 3138 / 1693 = 1.85x |

Stress LP (pr1002-shaped, 6002 x 5263), Devex: 0.770 / 0.110 = 7.0x time and 2271 / 1844 = 1.23x
iterations. DSE: 4.5x time and 0.88x iterations.

**G1.2 passes** with both pricings. The Plan B decision point (MASTER §8.3) is not triggered.

Caveat: the small LPs (ADLITTLE, SC205, SHARE2B) run in about 1–3 ms, so their time ratios are
dominated by timer noise. The totals are dominated by 25FV47 and GROW22.

## G1.3 sizes

The sprint file is inconsistent about the size of the stress LP. M1.6 asks for n degree rows plus
5000 random rows, which gives 6002 x 5263. G1.3 says "~2000 rows x ~4000 cols". Both were run:

```
stress_pr1002    6002   5263  optimal  2.4509341667e+05  iters=2271 time_s=0.770 iters/s=2951   (devex)
stress_pr1002    2002   5263  optimal  2.4258633333e+05  iters=1689 time_s=0.121 iters/s=13951  (devex, --stress-rows 1000)
HiGHS simplex_nopresolve on the 2002-row LP: 1878 iterations, 0.039 s
```

Both objectives are below the pr1002 optimum (259045), as a valid relaxation bound must be.
`used_artificial_bounds=0`: the TSP-shaped LP starts dual feasible.
