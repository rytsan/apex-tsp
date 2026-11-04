# SPRINT 2 — CUT ENGINE (SEC + blossom, pool, cut-and-resolve loop)

> Paste **00_MASTER_PROMPT.md** together with this file.
**Existing (stable):** Sprints 0–1 (reader, candidate graph, primal 2-opt, CSC, LU, eta,
bounded dual simplex, MPS/Netlib harness, CLI, checkpoint).
**Builds:** Layer 4 (cut engine) + the cut-and-resolve loop on top of the root LP.
**Modules:** M2.1 → M2.6, one per response, in order.

---

## M2.1 — Support graph + components (`src/cuts/support.{hpp,cpp}`)

- `SupportGraph build_support(const Lp&, double tol)` — edges with `x_e > tol` (default 1e-9),
  capacity = `x_e`.
- `std::vector<std::vector<uint32_t>> connected_components(const SupportGraph&);` (iterative
  DFS, no recursion on 100k nodes).
- Integral-but-disconnected solutions => one SEC per component (violation `2 - x(delta(S))`).
- Tests: `T_components_disconnected_integral` (construct a 3-cycle + 4-cycle fractional-free
  LP solution => 2 cuts), `T_support_capacity_exact` (capacities equal LP values).

## M2.2 — Padberg–Rinaldi shrinking (`src/cuts/support.{hpp,cpp}`, extending M2.1)

- Implement **iterative shrinking** on the support graph before any min-cut computation:
  - Default safe rule (MASTER §6): contract edge (i,j) whenever `x_ij = 1`
    (safety proof: `x(delta({i,j})) = 2` under degree constraints, and for every
    S ⊇ {i,j}, `x(delta(S)) >= x(delta({i,j}))` — no violated SEC can contain both endpoints).
    Prove via `T_shrinking_safe`.
  - Optional aggressive rules (e.g., degree-2 chains) may be enabled **only after**
    `T_shrinking_safe` passes on >= 100 random fractional solutions with n <= 14 where ALL SECs
    are enumerable by brute force; cite Padberg & Rinaldi (1990) in the test comment.
- Shrunken graph keeps a mapping supernode -> original nodes for cut expansion back to the
  original space.
- Tests: `T_shrinking_safe` (property test above), `T_shrinking_expands_correctly`
  (expanded cut is a valid node subset of the original graph).

## M2.3 — Min-cut / max-flow (`src/cuts/mincut.{hpp,cpp}`)

- `class StoerWagner` — global min-cut on the (shrunken) support graph with capacities `x_e`,
  O(n*m + n^2 log n) with a pairing/heap-based maximum-adjacency phase. Own code.
- `class Dinic` — max-flow for pairwise separation; `min_cut_between(s, t)` returning the cut
  side S (used by the separation heuristics below).
- Separation strategy per round:
  1. components (M2.1) — integral case;
  2. shrink (M2.2), run Stoer-Wagner: if `x(delta(S)) < 2 - 1e-5` => add SEC;
  3. guided pairwise search: pick endpoint pairs from nodes with fractional degree / incident
     edges with `x_e` near 1, run Dinic min-cut between them (budget: `max_pairs_per_round`,
     default 64), add every distinct violated cut found.
- Tests: `T_stoer_wagner_known_cuts` (>= 10 graphs with hand-computed min cuts),
  `T_dinic_known_flows`, `T_sec_separation_finds_planted` (fractional solution with a planted
  violated SEC on n = 100 is found in <= 3 rounds).

## M2.4 — Blossom / 2-matching heuristic separator (`src/cuts/blossom.{hpp,cpp}`)

- Inequality (MASTER §6, corrected): handle H, odd tooth set T ⊆ delta(H):
  `x(delta(H) \ T) - x(T) >= 1 - |T|`; violated when below `1 - |T| - 1e-5`.
- Heuristic separation (default, marked `heuristic`): handles = connected components of the
  fractional support for several thresholds (Padberg–Hong / Grötschel–Holland); optimal odd
  tooth set per handle; every candidate re-verified exactly against the LP values.
- Exact Padberg–Rao separation (minimum odd cut on the edge-split auxiliary graph, Gomory–Hu
  tree via Gusfield) behind `--sep-blossom=full`.
- Tests: `T_blossom_planted_violation` (planted violation detected on n <= 60),
  `T_blossom_never_invalid` (fuzz: 1000 random support graphs — every emitted cut re-verified and
  satisfied by random tours; invalid emissions = failure), `T_blossom_padberg_rao_exact`
  (full mode matches brute force).

## M2.5 — Cut pool (`src/cuts/pool.{hpp,cpp}`)

- `class CutPool` — canonical representation (SEC: sorted node list; blossom: handle + teeth),
  64-bit hash dedup, insertion from all separators, **aging** (a cut inactive — zero dual — for
  `age_limit` re-solves, default 10, leaves the LP back into the pool), pool stats in telemetry.
- Thread-safe (parallel separation merges into the pool under a lock or lock-free bag; merge
  order deterministic given seed).
- Tests: `T_pool_dedup_aging`, `T_pool_deterministic_merge`.

## M2.6 — Cut-and-resolve loop (`src/bnc/root_loop.{hpp,cpp}`)

- Loop: solve root LP (dual simplex warm start) => run separators **in order** components =>
  SEC (shrinking + global + guided) => blossom => merge into pool => add violated rows
  (violation > 1e-5) => warm-start re-solve; stop when no family yields a violated cut.
- Assert (debug): adding `>=` rows with zero-cost slacks keeps dual feasibility; objective is
  non-decreasing across re-solves.
- Root RCF pass after convergence (MASTER §5.5, root-scoped = permanent).
- Emit structured log: rounds, cuts per family, root LP value, time per phase.

---

## SPRINT 2 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G2.1 | Band E2: pr1002 root LP (degree + separated cuts, **no branching**) | within 97–99.5% of 259045; above the optimum = bug |
| G2.2 | Root gap with cuts, no branching, on pr1002 and fl3795 | <= 2% on both |
| G2.3 | Root loop time budget | pr1002 <= 10 min; fl3795 <= 20 min (report actual) |
| G2.4 | §8.2 tests for this sprint (`T_components_*`, `T_shrinking_*`, `T_stoer_*`, `T_dinic_*`, `T_sec_*`, `T_blossom_*`, `T_pool_*`, `T_root_*`) | 100% green, output pasted |
| G2.5 | Root LB honesty cross-check (MASTER §8.3c) | LB <= UB(best known) on all runs; report LP vs. best-known ratio |
| G2.6 | ASan + UBSan clean | 0 findings |

**Deliverables:** file tree diff, full code, root-loop transcripts for pr1002/fl3795 (rounds,
cuts per family per round, final LP value vs. optimum), README "Sprint 2 results (hardware: ...)".
