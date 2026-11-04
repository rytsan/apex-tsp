# SPRINT 4 — PRIMAL ENGINE, DECOMPOSITION, PARALLELISM, MEMORY

> Paste **00_MASTER_PROMPT.md** together with this file.
**Existing (stable):** Sprints 0–3 (vertical slice, LP engine, cut engine, B&C with proofs).
**Builds:** Layer 1.5 completion (LK-lite, ILS), Layer 2 (KD-tree decomposition + patching,
heuristic), Held-Karp Lagrangian bound, memory-aware scheduling + enforcement, hypersparsity,
Forrest-Tomlin.
**Modules:** M4.1 → M4.6, one per response, in order.

---

## M4.1 — LK-lite + Iterated Local Search (`src/primal/lk_lite.{hpp,cpp}`)

- Extend the Sprint 0 primal engine: variable-depth LK-style moves restricted to candidate
  neighbor lists (sequential 3-opt/4-opt moves, tabu-free, don't-look bits), plus **iterated
  local search**: perturbation = double-bridge, restart from best, acceptance = better-or-equal
  with periodic kicks. All deterministic under `--seed`.
- Runs in a **background thread** for the whole engine lifetime; publishes UB improvements
  atomically (B&C tree and RCF react at node boundaries).
- Tests: `T_ils_improves_monotone` (on berlin52/a280, best cost never regresses across 100
  iterations), `T_lk_deterministic`, `T_background_thread_no_races` (TSan clean).

## M4.2 — Primal heuristics inside B&C (`src/bnc/primal_heur.hpp`)

- **Diving:** after each node LP, if fractional, fix variables with `x_e > 1 - 1e-6` and
  re-solve (max 5 dive rounds/node) looking for early integral solutions.
- **RIH-lite:** once per root, fix all `x_e = 1` variables, run the primal engine on the fixed
  subgraph + reconnection, accept as UB if better.
- Both are heuristic (UB only) and marked as such in logs.
- Tests: `T_diving_finds_tour_small` (on eil51 within 1000 nodes processed, tour found),
  `T_rih_never_worsens_ub`.

## M4.3 — Spatial decomposition + patching (scalable mode) (`src/decomp/`)

- `src/decomp/kdtree.{hpp,cpp}` — balanced KD-tree, leaves of 500–800 nodes (CLI-tunable).
- `src/decomp/leaf_solver.{hpp,cpp}` — each leaf solved by the primal engine (and, when
  `n_leaf <= --leaf-exact-limit` (default 300), by the exact mini-B&C path for high-quality
  local tours). `tbb::parallel_for` over leaves.
- `src/decomp/patching.{hpp,cpp}` — merge sibling clusters: portals = k = 4 nearest pairs
  across the sibling boundary; stitch by optimal assignment on the portal matching (use your
  own Hungarian/auction implementation — no external solver) + K-opt cleanup across fragment
  borders. Output: a full valid tour => candidate global UB.
- **HARD RULE (MASTER §2, directive 5 — bound honesty):** this layer produces UB and candidate-graph statistics ONLY.
  No local bound may ever feed pruning. Every entry point carries `// heuristic:` markers and
  telemetry reports this phase as `phase: decomposition (heuristic)`.
- Tests: `T_patch_produces_valid_tour` (permutation valid, cost matches recomputation) on
  20 random instances + pla85900, `T_leaf_parallel_deterministic_option`.

## M4.4 — Held-Karp Lagrangian bound (`src/hk/held_karp.{hpp,cpp}`)

- 1-tree relaxation + node multipliers, subgradient ascent (step rule: Polyak with decay;
  iteration cap and time budget). Deterministic.
- Uses: (a) warm LB for scalable mode before the root LP (`lb_source: lagrangian` in
  telemetry); (b) **Plan B** for exact mode if the Sprint 1 gate failed — in that case,
  exact mode = Lagrangian bound + edge-branching B&B on 1-tree edges (document the mode
  switch in README); (c) cross-check: LP bound < converged HK bound => bug flag.
- Tests: `T_hk_bound_valid` (HK LB <= optimal on eil51/berlin52/a280/pr1002),
  `T_hk_convergence` (gap to subtour LP within documented factor on pr1002).

## M4.5 — Memory-aware scheduling + real enforcement (`src/app/mem.{hpp,cpp}`)

- **Enforcement:** Windows Job Objects (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`) /
  Linux `setrlimit(RLIMIT_AS)` + optional cgroup v2 notes; applied at startup from
  `--mem-limit-mb`.
- **Scheduler:** LP concurrency token pool sized by `free_budget = mem_limit - baseline_usage`
  ÷ `lp_working_set_estimate` (recomputed from live factorization sizes every refactorization);
  default cap 6 concurrent LPs at 4 GB. When the budget is exhausted: checkpoint + graceful
  `MEM_LIMIT` shutdown (never an OS kill as the primary path).
- Peak-RAM probe: page-fault based (Windows `GetProcessMemoryInfo`, Linux VmHWM), reported in
  telemetry and final JSON.
- Tests: `T_mem_limit_graceful` (run with 256 MB ceiling on pla85900 => MEM_LIMIT report +
  resumable checkpoint), `T_lp_concurrency_budget` (observed concurrent LPs never exceed
  computed budget).

## M4.6 — LP engine upgrades: Forrest-Tomlin + hypersparsity (`src/lp/ft_update.{hpp,cpp}`)

- **Forrest-Tomlin** basis update replacing PFI behind the same `BasisUpdate` interface
  (PFI kept as `--basis-update=pfi` fallback). Refactorization scheduler unchanged (M1.3).
- **Hypersparsity:** sparse FTRAN/BTRAN paths with hypermarkowitz-style spike handling;
  measure with/without on the stress LP.
- A/B benchmark (paste table): stress LP + fl3795 root LP — PFI vs. FT: iterations, time,
  refactorization count, peak factorization memory.
- Tests: `T_ft_equivalence` (FT and PFI produce identical objectives on 50 random LPs),
  `T_ft_residual` (`||Bx-b||/||b|| <= 1e-8` after 200 sequential updates),
  `T_hyper_sparse_ftran` (solution identical to dense path, fewer nnz touched — instrumented).

---

## SPRINT 4 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G4.1 | Band D: standalone primal on mona-lisa100K | gap <= 3% vs. 5757191 in <= 45 min single-thread (stretch: 2%); paste tour hash + cost |
| G4.2 | Band C: scalable mode on pla85900 | gap <= 2% in <= 8 h; peak RAM <= ceiling |
| G4.3 | Band C: scalable mode on mona-lisa100K | gap <= 2% vs. 5757191 in <= 8 h (LB = max(root LP, HK); report both) |
| G4.4 | Decomposition honesty audit | grep audit shows zero non-`heuristic` uses of local bounds in pruning paths |
| G4.5 | FT vs. PFI A/B table pasted | FT adopted by default unless it loses >= 10% on both benchmarks (justify per C0.7) |
| G4.6 | Memory enforcement | `T_mem_limit_graceful` + `T_lp_concurrency_budget` green |
| G4.7 | Regression | Band A instances (eil51, berlin52, a280, pr1002, fl3795) still prove optimality within Sprint 3 limits |
| G4.8 | TSan + ASan + UBSan clean | 0 findings |

**Deliverables:** file tree diff, full code, benchmark transcripts (G4.1–G4.3 with hardware
context), A/B table, README "Sprint 4 results (hardware: ...)".
