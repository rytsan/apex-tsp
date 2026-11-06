# Sprint 3 — branch-and-cut transcripts

Hardware: AMD Ryzen 5 3600X, WSL2, GCC 13.3, preset `benchmark`, commit `e9f7235`.
Command: `root_bench --instance data/<name>.tsp --ref <opt> --tree [--time-limit T --parallel-lps 2 --checkpoint DIR]`.

## Small instances (serial, proven)

```
SUMMARY eil51 n=51 status=OPTIMAL_PROVEN ub=426 lb=426.0000 lb_int=426 gap=0.0000% ref=426
  certificate: ub == recomputed tour cost: OK   lb <= ref: OK   ub == ref: yes
  root_lb=426.0000 nodes=2 branched=1 pruned=1 infeasible=0 integral=1 depth=1 ub_improvements=2
  warm=1 cold=0 warm_dual_infeasible=0  edges=252  rcf_root fixed0=160 fixed1=14
  heuristic calls=1 time=0.00s   strong branching: candidates=5 pivots=60 max_per_node=60
  parallel_lps=1 batches=0 checkpoints=0
  time: lp=0.01s (36.5%) sep=0.01s pricing=0.00s total=0.02s nodes/s=98.23 rss=4.4MB

SUMMARY berlin52 n=52 status=OPTIMAL_PROVEN ub=7542 lb=7542.0000 lb_int=7542 gap=0.0000% ref=7542
  certificate: ub == recomputed tour cost: OK   lb <= ref: OK   ub == ref: yes
  root_lb=7542.0000 nodes=1 branched=0 pruned=0 infeasible=0 integral=1 depth=0 ub_improvements=1
  warm=0 cold=0 warm_dual_infeasible=0  edges=292  rcf_root fixed0=210 fixed1=26
  heuristic calls=0 time=0.00s   strong branching: candidates=0 pivots=0 max_per_node=0
  parallel_lps=1 batches=0 checkpoints=0
  time: lp=0.00s (71.6%) sep=0.00s pricing=0.00s total=0.00s nodes/s=1251.16 rss=4.1MB

SUMMARY a280 n=280 status=OPTIMAL_PROVEN ub=2579 lb=2579.0000 lb_int=2579 gap=0.0000% ref=2579
  certificate: ub == recomputed tour cost: OK   lb <= ref: OK   ub == ref: yes
  root_lb=2576.5238 nodes=9 branched=4 pruned=4 infeasible=0 integral=1 depth=3 ub_improvements=4
  warm=8 cold=0 warm_dual_infeasible=0  edges=3420  rcf_root fixed0=911 fixed1=66
  heuristic calls=4 time=0.00s   strong branching: candidates=25 pivots=189 max_per_node=60
  parallel_lps=1 batches=0 checkpoints=0
  time: lp=0.05s (31.9%) sep=0.02s pricing=0.01s total=0.15s nodes/s=61.68 rss=6.6MB

```

## pr1002 (1800 s, 2 parallel LPs)

```
t=3.555 event=ub_improved nodes=1 ub=264305 source=lp_guided
t=5.281 event=ub_improved nodes=9 ub=263281 source=lp_guided
t=6.400 event=ub_improved nodes=17 ub=261276 source=lp_guided
t=6.400 event=ub_improved nodes=17 ub=260550 source=lp_guided
t=7.229 event=ub_improved nodes=21 ub=260156 source=lp_guided
t=10.846 event=ub_improved nodes=47 ub=259938 source=lp_guided
t=12.262 event=ub_improved nodes=57 ub=259688 source=lp_guided
t=24.491 event=ub_improved nodes=131 ub=259614 source=lp_guided
t=28.341 event=ub_improved nodes=155 ub=259519 source=lp_guided
t=36.150 event=ub_improved nodes=203 ub=259356 source=lp_guided
t=41.151 event=ub_improved nodes=235 ub=259259 source=lp_guided
t=559.081 event=ub_improved nodes=2027 ub=259146 source=lp_guided
t=1429.153 event=ub_improved nodes=4081 ub=259088 source=lp_guided
t=1492.737 event=ub_improved nodes=4237 ub=259051 source=lp_guided
t=1804.506 event=FINISHED status=TIME_LIMIT ub=259051 lb=259020.8200 gap_pct=0.0117 nodes=4871 open=3252 max_depth=43 root_lb=258402.4966 verification_pricing_calls=13183 priced_columns=13348 certificate=none parallel_lps=2 open_bytes=281892930 pool_cuts=3886 edges=18604 checkpoints=6 lp_s=1106.78 sep_s=634.36 pricing_s=37.48 total_s=1804.50

SUMMARY pr1002 n=1002 status=TIME_LIMIT ub=259051 lb=259020.8200 lb_int=259021 gap=0.0117% ref=259045
  certificate: ub == recomputed tour cost: OK   lb <= ref: OK   ub == ref: no
  root_lb=258402.4966 nodes=4871 branched=4069 pruned=815 infeasible=2 integral=1 depth=43 ub_improvements=14
  warm=4870 cold=0 warm_dual_infeasible=0  edges=18604  rcf_root fixed0=434 fixed1=1
  heuristic calls=4376 time=10.74s   strong branching: candidates=15183 pivots=243708 max_per_node=60
  parallel_lps=2 batches=2435 checkpoints=6
  time: lp=1106.78s (61.3%) sep=634.36s pricing=37.48s total=1804.50s nodes/s=2.70 rss=1152.3MB
```

## fl3795 (3600 s, 2 parallel LPs)

```
t=574.595 event=ub_improved nodes=1 ub=31368 source=lp_guided
t=605.823 event=ub_improved nodes=3 ub=29551 source=lp_guided
t=1329.654 event=ub_improved nodes=35 ub=29271 source=lp_guided
t=3610.325 event=FINISHED status=TIME_LIMIT ub=29271 lb=28571.2770 gap_pct=2.3905 nodes=93 open=94 max_depth=46 root_lb=28570.8225 verification_pricing_calls=1448 priced_columns=158367 certificate=none parallel_lps=2 open_bytes=7769965 pool_cuts=13854 edges=168337 checkpoints=10 lp_s=2569.17 sep_s=2885.97 pricing_s=370.67 total_s=3610.31

SUMMARY fl3795 n=3795 status=TIME_LIMIT ub=29271 lb=28571.2770 lb_int=28572 gap=2.3905% ref=28772
  certificate: ub == recomputed tour cost: OK   lb <= ref: OK   ub == ref: no
  root_lb=28570.8225 nodes=93 branched=93 pruned=0 infeasible=0 integral=0 depth=46 ub_improvements=3
  warm=92 cold=0 warm_dual_infeasible=2  edges=168337  rcf_root fixed0=23 fixed1=0
  heuristic calls=93 time=2.57s   strong branching: candidates=376 pivots=5567 max_per_node=60
  parallel_lps=2 batches=46 checkpoints=10
  time: lp=2569.17s (71.2%) sep=2885.97s pricing=370.67s total=3610.31s nodes/s=0.03 rss=1902.9MB
```

Peak sampled RSS of fl3795: 1556 MB at t=3460 s (sampler, 10 s period); process-reported peak 1902.9 MB.
