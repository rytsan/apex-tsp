# Sprint 2 — root cut-and-resolve transcripts

Hardware: AMD Ryzen 5 3600X (5 physical cores visible under WSL2), 25 GB RAM, Linux 6.18 WSL2, GCC 13.3,
preset `benchmark`. Single thread, seed 1.

Command: `build/benchmark/root_bench --instance data/<name>.tsp --ref <best known>`.
UB comes from the Sprint 0 primal engine (Hilbert + 2-opt/Or-opt). Blossom separation is in heuristic mode.

Field meanings:
- `lp`: root LP value over the current candidate graph.
- `global_lb`: Farley bound, valid for the complete graph. It uses exact reduced costs of all
  n(n-1)/2 edges. Once no edge has a negative reduced cost, `global_lb == lp_value`.
- `comp` / `sec` / `blossom`: new cuts found per family in that round.
- `pool`: cuts brought back from the pool.
- `aged`: non-binding cuts removed from the LP.

Stopping rules:
- `converged`: no violated cut and no negative-reduced-cost edge.
- `tailing_off`: the LP gained less than 1e-4 * |z| over the last 50 rounds. Violated cuts may still
  exist, but the reported bound is fully priced and therefore valid.

### pr1002

```
t=0.003 event=root_start instance=pr1002 n=1002 edges=5263 ub_2opt=279341 ref=259045 blossom=heuristic
... 97 root_round events, sampled every 25th + last:
t=0.045 event=root_round round=0 lp=240877.5000 rows=1111 comp=109 sec=0 blossom=0 pool=0 aged=0 iters=1507 lp_s=0.040 sep_s=0.001
t=0.439 event=root_round round=25 lp=257551.5314 rows=1417 comp=0 sec=0 blossom=27 pool=0 aged=0 iters=1 lp_s=0.004 sep_s=0.013
t=1.383 event=root_round round=50 lp=258172.6423 rows=1466 comp=0 sec=9 blossom=0 pool=0 aged=92 iters=45 lp_s=0.010 sep_s=0.022
t=2.554 event=root_round round=75 lp=258371.8978 rows=1490 comp=0 sec=0 blossom=4 pool=0 aged=0 iters=1 lp_s=0.005 sep_s=0.030
t=3.500 event=root_round round=96 lp=258402.4966 rows=1466 comp=0 sec=0 blossom=0 pool=0 aged=0 iters=7 lp_s=0.013 sep_s=0.032
... 11 root_pricing events, first 3 + last 3:
t=0.047 event=root_pricing round=1 negative=2 added=2 edges=5265 global_lb=240479.5000
t=0.157 event=root_pricing round=2 negative=354 added=354 edges=5619 global_lb=168673.8958
t=0.327 event=root_pricing round=3 negative=312 added=312 edges=5931 global_lb=176893.8738
t=2.034 event=root_pricing round=9 negative=1 added=1 edges=6321 global_lb=258318.7397
t=2.340 event=root_pricing round=10 negative=10 added=10 edges=6331 global_lb=258088.7658
t=2.780 event=root_pricing round=11 negative=8 added=8 edges=6339 global_lb=257864.1106
t=3.505 event=root_done converged=true tailing_off=false rounds=97 lp_value=258402.4966 lp_scope=candidate_graph global_lb=258402.4966 lb_source=lp negative_rc_edges=0 pricing_rounds=11 priced_edges=1076 cuts_sec=482 cuts_blossom=352 cuts_component=228 rcf_fixed0=0 rcf_fixed1=0 lp_s=1.246 sep_s=1.885 pricing_s=0.236 total_s=3.501
```

```
SUMMARY pr1002 n=1002 status=optimal converged=1 tailing_off=0 rounds=97
  lp_value(candidate graph) = 258402.4966   ratio_to_ref = 99.7520%   gap_vs_ref = 0.2480%
  global_lb (Farley, complete graph) = 258402.4966  lb_int = 258403  ratio_to_ref = 99.7520%  negative_rc_edges = 0
  ub_2opt = 279341   gap_vs_ub = 7.4957%   honesty: global_lb <= ref: OK, lp_value <= ref: OK
  cuts: component=228 sec=482 blossom=352  pool size=1007 in_lp=464 aged_out=638 dup=55
  pricing: rounds=11 priced_edges=1076 final_edges=6339
  rcf: fixed0=0 fixed1=0   time: lp=1.25s sep=1.88s pricing=0.24s total=3.50s  rss=10.6MB
```

### fl3795

```
t=0.015 event=root_start instance=fl3795 n=3795 edges=20397 ub_2opt=32982 ref=28772 blossom=heuristic
... 415 root_round events, sampled every 25th + last:
t=0.672 event=root_round round=0 lp=24288.5000 rows=3918 comp=123 sec=0 blossom=0 pool=0 aged=0 iters=7428 lp_s=0.650 sep_s=0.004
t=2.527 event=root_round round=25 lp=27828.2500 rows=4329 comp=0 sec=62 blossom=0 pool=0 aged=0 iters=1 lp_s=0.008 sep_s=0.024
t=4.963 event=root_round round=50 lp=28467.2500 rows=4362 comp=0 sec=2 blossom=0 pool=0 aged=30 iters=10 lp_s=0.012 sep_s=0.027
t=7.856 event=root_round round=75 lp=28498.0000 rows=4716 comp=0 sec=0 blossom=44 pool=0 aged=0 iters=1 lp_s=0.014 sep_s=0.070
t=13.123 event=root_round round=100 lp=28518.8405 rows=4793 comp=0 sec=1 blossom=0 pool=0 aged=130 iters=18 lp_s=0.036 sep_s=0.121
t=20.483 event=root_round round=125 lp=28525.8305 rows=4962 comp=0 sec=10 blossom=0 pool=8 aged=0 iters=167 lp_s=0.099 sep_s=0.189
t=30.528 event=root_round round=150 lp=28529.7473 rows=5034 comp=0 sec=6 blossom=0 pool=1 aged=176 iters=183 lp_s=0.160 sep_s=0.266
t=44.677 event=root_round round=175 lp=28535.5152 rows=5251 comp=0 sec=4 blossom=0 pool=2 aged=0 iters=106 lp_s=0.149 sep_s=0.370
t=62.941 event=root_round round=200 lp=28540.7477 rows=5137 comp=0 sec=10 blossom=0 pool=10 aged=172 iters=511 lp_s=0.628 sep_s=0.432
t=81.386 event=root_round round=225 lp=28543.7554 rows=5331 comp=0 sec=0 blossom=20 pool=0 aged=0 iters=4 lp_s=0.039 sep_s=0.428
t=101.798 event=root_round round=250 lp=28547.8838 rows=5248 comp=0 sec=6 blossom=0 pool=4 aged=128 iters=572 lp_s=0.698 sep_s=0.493
t=127.511 event=root_round round=275 lp=28551.3882 rows=5576 comp=3 sec=0 blossom=0 pool=6 aged=0 iters=141 lp_s=0.384 sep_s=0.278
t=157.875 event=root_round round=300 lp=28553.4107 rows=5495 comp=0 sec=0 blossom=14 pool=0 aged=272 iters=30 lp_s=0.087 sep_s=0.763
t=189.531 event=root_round round=325 lp=28559.6493 rows=5525 comp=0 sec=0 blossom=16 pool=0 aged=0 iters=5 lp_s=0.108 sep_s=0.897
t=224.138 event=root_round round=350 lp=28561.6579 rows=5408 comp=0 sec=0 blossom=14 pool=0 aged=239 iters=1 lp_s=0.073 sep_s=0.874
t=265.002 event=root_round round=375 lp=28567.1922 rows=5728 comp=0 sec=0 blossom=15 pool=0 aged=0 iters=1 lp_s=0.084 sep_s=0.906
t=309.188 event=root_round round=400 lp=28568.1506 rows=5303 comp=0 sec=0 blossom=12 pool=2 aged=241 iters=179 lp_s=0.449 sep_s=1.017
t=334.122 event=root_round round=414 lp=28568.5400 rows=5773 comp=0 sec=155 blossom=0 pool=187 aged=0 iters=565 lp_s=1.654 sep_s=1.111
... 110 root_pricing events, first 3 + last 3:
t=0.789 event=root_pricing round=1 negative=23 added=23 edges=20420 global_lb=25153.0000
t=0.863 event=root_pricing round=2 negative=2 added=2 edges=20422 global_lb=25459.0000
t=0.930 event=root_pricing round=3 negative=7 added=7 edges=20429 global_lb=25656.7500
t=307.076 event=root_pricing round=108 negative=1 added=1 edges=91884 global_lb=28568.1438
t=309.254 event=root_pricing round=109 negative=4077 added=2000 edges=93884 global_lb=-12192.3813
t=327.278 event=root_pricing round=110 negative=604 added=604 edges=94488 global_lb=26945.7545
t=335.151 event=root_done converged=false tailing_off=true rounds=415 lp_value=28568.5411 lp_scope=candidate_graph global_lb=28568.5411 lb_source=lp negative_rc_edges=0 pricing_rounds=110 priced_edges=74091 cuts_sec=4980 cuts_blossom=1968 cuts_component=539 rcf_fixed0=0 rcf_fixed1=0 lp_s=101.985 sep_s=183.421 pricing_s=0.076 total_s=335.135
```

```
SUMMARY fl3795 n=3795 status=optimal converged=0 tailing_off=1 rounds=415
  lp_value(candidate graph) = 28568.5411   ratio_to_ref = 99.2929%   gap_vs_ref = 0.7071%
  global_lb (Farley, complete graph) = 28568.5411  lb_int = 28569  ratio_to_ref = 99.2929%  negative_rc_edges = 0
  ub_2opt = 32982   gap_vs_ub = 13.3814%   honesty: global_lb <= ref: OK, lp_value <= ref: OK
  cuts: component=539 sec=4980 blossom=1968  pool size=6180 in_lp=1978 aged_out=33574 dup=1307
  pricing: rounds=110 priced_edges=74091 final_edges=94488
  rcf: fixed0=0 fixed1=0   time: lp=101.99s sep=183.42s pricing=0.08s total=335.14s  rss=389.4MB
wall_s=335.18 peak_rss_kb=398740
```
