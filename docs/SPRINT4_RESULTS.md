# Sprint 4 — gate transcripts

Hardware: AMD Ryzen 5 3600X (5 physical cores visible under WSL2), 25 GB RAM, Linux 6.18 WSL2, GCC 13.3,
preset `benchmark`. Binary: `apex_tsp` at commit `5363ff5`. Seed 1 everywhere.

## G4.1 — Band D, standalone primal, mona-lisa100K, 1 thread, 45 min

`apex_tsp --instance data/mona-lisa100K.tsp --mode scalable --threads 1 --time-limit 2700`

```
t=159.277 event=construct phase="decomposition (heuristic)" heuristic=true leaves=182 levels=9 leaf_min=307 leaf_max=790 exact_leaves=0 leaf_sum=5811833 stitch_cost=-1725 reeval_gain=33687 ub=5776421 ms=158158.368
t=346.562 event=held_karp lb_source=lagrangian lb=5751054.1586 lb_int=5751055 iterations=2000 ascent_ms=169559.225 exact_ms=17437.446
t=2700.025 event=polish heuristic=true ub=5765918 start=5776421 parallel=false segments=1 rounds=1 iterations=1906976 ms=2353462.991
t=2700.041 event=done status=done mode=scalable ub=5765918 lb=5751055 gap_pct=0.2578 lb_source=lagrangian wall_s=2700.013 ram_peak_bytes=55549952
tour sha256: cfaf3c5f2e5eea5689eeda120e7f31a4f382faf1f59071b6f3886338cdac3e8a
```

## G4.2 — Band C, pla85900, 5 threads, 600 s (limit 8 h), --mem-limit-mb 4096

```
t=20.414 event=construct phase="decomposition (heuristic)" heuristic=true leaves=146 levels=8 leaf_min=299 leaf_max=796 exact_leaves=1 leaf_sum=148238012 stitch_cost=-15142 reeval_gain=3624400 ub=144598470 ms=20086.840
t=90.873 event=held_karp lb_source=lagrangian lb=141588257.6911 lb_int=141588258 iterations=952 ascent_ms=57956.943 exact_ms=12398.887
t=600.173 event=polish heuristic=true ub=143332990 start=144598470 parallel=true segments=9 rounds=10 iterations=26303424 ms=509299.328
t=600.188 event=done status=done mode=scalable ub=143332990 lb=141588258 gap_pct=1.2173 lb_source=lagrangian wall_s=600.175 ram_peak_bytes=80814080
tour sha256: 85643c26c400831756eac15244c1103baf0a821a26d32881404e26e8a1e6320a
```

## G4.3 — Band C, mona-lisa100K, 5 threads, 600 s (limit 8 h)

```
t=49.788 event=construct phase="decomposition (heuristic)" heuristic=true leaves=182 levels=9 leaf_min=307 leaf_max=790 exact_leaves=0 leaf_sum=5811833 stitch_cost=-1725 reeval_gain=33687 ub=5776421 ms=49171.069
t=122.709 event=held_karp lb_source=lagrangian lb=5658314.3517 lb_int=5658315 iterations=408 ascent_ms=55140.856 exact_ms=17370.865
t=600.063 event=polish heuristic=true ub=5764699 start=5776421 parallel=true segments=10 rounds=10 iterations=11110160 ms=477353.991
t=600.072 event=done status=done mode=scalable ub=5764699 lb=5658315 gap_pct=1.8454 lb_source=lagrangian wall_s=600.066 ram_peak_bytes=89038848
tour sha256: 162626d4c78dde7916825f6fa458938e4d1c3f938789c0d2fa8f7212af88826d
```

Root LP: not run on 100K nodes (the exact-mode root of fl3795 alone takes minutes; the LP for
n = 100000 is outside this sprint's scalable budget), so LB = HK.

## G4.4 — decomposition honesty audit

```
## 1. decomposition / primal entry points carry heuristic markers
src/decomp/kdtree.hpp:3:// heuristic: spatial decomposition for the scalable mode (UB / candidate statistics only).
src/decomp/kdtree.hpp:25:// heuristic (M4.7): density-aware split. The cut goes through the widest empty gap along the
src/bnc/primal_heur.hpp:3:// Primal heuristics inside B&C (M4.2). heuristic: both produce UB candidates only; nothing here
src/bnc/primal_heur.hpp:29:// heuristic: dive from a fractional node LP solution x (over the LP columns = g.edges). Each
src/bnc/primal_heur.hpp:39:// heuristic: RIH-lite. Fragments from the x_e = 1 edges, reconnected greedily over the candidate
src/decomp/patching.hpp:3:// heuristic: patching of leaf tours into one tour (scalable mode). Produces a UB candidate and
src/decomp/patching.hpp:5:// directive 5). Telemetry phase: "decomposition (heuristic)".
src/decomp/patching.hpp:42:// heuristic: merges cycles a and b (disjoint, global ids) of the two sides of a split. Portals:
src/decomp/patching.hpp:52:// heuristic: ILS on one cluster cycle (global ids, rewritten in place) on its own candidate
src/decomp/patching.hpp:57:// heuristic: density tree (or KD) leaves solved in parallel, merged bottom-up level by level
src/decomp/leaf_solver.hpp:3:// heuristic: leaf tours for the scalable mode. UB only: no leaf bound is ever reported or used.
src/decomp/leaf_solver.hpp:29:// heuristic: one Hamiltonian cycle (global ids) per leaf of `tree`, indexed like `tree` (empty
src/decomp/pipeline.hpp:3:// heuristic: primal pipeline (M4.7). UB construction and improvement only; nothing here is a bound.
src/decomp/pipeline.hpp:26:// heuristic
src/decomp/pipeline.hpp:47:// heuristic

## 2. pruning paths include no decomposition / Held-Karp / primal bound code
(none)

## 3. every pruning / fixing decision and the bound it uses
src/bnc/rcf.cpp:7:std::vector<BoundChange> rcf_fixings(const SimplexResult& res, double bound, int64_t ub) {
src/bnc/rcf.cpp:20:void apply_root_rcf(CandidateGraph& g, const SimplexResult& final, double global_lb, int64_t ub, uint32_t& fixed0,
src/bnc/rcf.cpp:22:    for (const auto& f : rcf_fixings(final, global_lb, ub)) {
src/bnc/root_loop.cpp:182:    PricingFilter filter{&root_yp_, root_lb_, cutoff_};
src/bnc/root_loop.cpp:340:            out.cutoff = true;  // the subtree cannot hold a tour better than the cutoff
src/bnc/root_loop.cpp:365:        PricingFilter filter{&root_yp_, root_lb_, cutoff_};
src/bnc/root_loop.cpp:387:        apply_root_rcf(g, out.final, out.global_lb, cfg.ub, out.rcf_fixed0, out.rcf_fixed1);
src/bnc/tree.cpp:40:bool prunable(double bound, int64_t ub) { return std::ceil(bound - 1e-6) >= static_cast<double>(ub); }
src/bnc/tree.cpp:151:    if (prunable(r.global_lb, ub)) { o.kind = tour ? NodeOutcome::Kind::Integral : NodeOutcome::Kind::Pruned; return; }
src/bnc/tree.cpp:168:    for (const auto& f : rcf_fixings(r.final, r.global_lb, ub)) {
src/bnc/tree.cpp:402:        apply_root_rcf(g_, out_.root.final, out_.root_lb, out_.ub, out_.root.rcf_fixed0, out_.root.rcf_fixed1);
src/bnc/tree.cpp:409:        apply_root_rcf(g_, out_.root.final, out_.root_lb, out_.ub, out_.root.rcf_fixed0, out_.root.rcf_fixed1);
src/bnc/tree.cpp:428:            if (prunable(node.dual_bound, out_.ub)) { ++out_.pruned; continue; }

## 4. bound identifiers (lb, global_lb, root_lb, dual_bound, held_karp, one_tree) in decomposition / primal code
(none)
```

## G4.5 — Forrest-Tomlin vs PFI (A/B, M4.6)

| Benchmark | Mode | Iterations | Refactorizations | Peak LU nnz | LP time | RSS |
|---|---|---|---|---|---|---|
| stress_pr1002 | PFI | 2271 | 25 | 54806 | 0.81 s | 20.6 MB |
| stress_pr1002 | FT | 2404 | 26 | 56179 | 0.93 s | 22.7 MB |
| stress_pr1002 | FT + hypersparse | 2404 | 26 | 56179 | 0.99 s | 22.6 MB |
| fl3795 root | PFI | 97704 | 1631 | 80366 | 234.1 s | 334 MB |
| fl3795 root | FT | 95571 | 1593 | 73002 | 173.2 s | 171 MB |
| fl3795 root | FT + hypersparse | 95571 | 1593 | 73002 | 179.5 s | 178 MB |

FT loses about 15% on the stress LP and wins 26% on fl3795: it does not lose >= 10% on both, so it stays the default.

## G4.7 — Band A regression, `apex_tsp --mode exact`

```
{"instance":"eil51","mode":"exact","ub":426,"lb":426,"gap_pct":0.000000,"lb_source":"lp","wall_s":0.195,"seed":1,"threads":1,"ram_peak_bytes":4919296,"status":"OPTIMAL_PROVEN"}
{"instance":"berlin52","mode":"exact","ub":7542,"lb":7542,"gap_pct":0.000000,"lb_source":"lp","wall_s":0.085,"seed":1,"threads":1,"ram_peak_bytes":4677632,"status":"OPTIMAL_PROVEN"}
{"instance":"a280","mode":"exact","ub":2579,"lb":2579,"gap_pct":0.000000,"lb_source":"lp","wall_s":0.722,"seed":1,"threads":1,"ram_peak_bytes":5664768,"status":"OPTIMAL_PROVEN"}
t=16.282 event=polish heuristic=true ub=260003 start=270673 parallel=false segments=1 rounds=1 iterations=640312 ms=16276.287
t=16.283 event=bnc_start ub=260003 tour_edges_added=1 edges=5264 parallel_lps=5 background_ils=true
t=1806.780 event=FINISHED status=TIME_LIMIT ub=259062 lb=259021.3987 gap_pct=0.0157 nodes=6732 open=5057 max_depth=27 root_lb=258368.3514 verification_pricing_calls=18033 priced_columns=1635 certificate=none parallel_lps=5 open_bytes=212558076 lp_working_set_bytes=13329896 max_concurrent_lps=5 min_lp_tokens=6 pool_cuts=3714 edge
{"instance":"pr1002","mode":"exact","ub":259062,"lb":259022,"gap_pct":0.015440,"lb_source":"lp","wall_s":1806.787,"seed":1,"threads":5,"ram_peak_bytes":906108928,"status":"TIME_LIMIT"}

t=39.726 event=polish heuristic=true ub=29012 start=29894 parallel=false segments=1 rounds=1 iterations=1386531 ms=37593.894
t=39.743 event=bnc_start ub=29012 tour_edges_added=5 edges=20402 parallel_lps=4 background_ils=true
t=3601.321 event=FINISHED status=TIME_LIMIT ub=28935 lb=28581.6139 gap_pct=1.2213 nodes=95 open=96 max_depth=7 root_lb=28569.7540 verification_pricing_calls=1461 priced_columns=109362 certificate=none parallel_lps=4 open_bytes=6882384 lp_working_set_bytes=225007588 max_concurrent_lps=4 min_lp_tokens=6 pool_cuts=12416 edges=12566
{"instance":"fl3795","mode":"exact","ub":28935,"lb":28582,"gap_pct":1.219976,"lb_source":"lp","wall_s":3601.336,"seed":1,"threads":4,"ram_peak_bytes":2207301632,"status":"TIME_LIMIT"}
```

pr1002 run: commit `1b041c3` (identical engine; `5363ff5` only renames two locals).

## G4.8 — sanitizers

```
[doctest] test cases:    111 |    111 passed | 0 failed | 0 skipped
[doctest] assertions: 604692 | 604692 passed | 0 failed |
asan exit=0
ASan/UBSan findings: 0
[doctest] test cases:    7 |    7 passed | 0 failed | 104 skipped
[doctest] assertions: 1674 | 1674 passed | 0 failed |
TSan warnings: 0
```
