# SPRINT 3 — GLOBAL BRANCH-AND-CUT (optimality proofs, exact mode)

> Paste **00_MASTER_PROMPT.md** together with this file.
**Existing (stable):** Sprints 0–2 (primal 2-opt, LP engine, separators, cut pool, root loop,
CLI, checkpoint v1).
**Builds:** Layer 5 (B&C tree, branching, RCF, verification pricing, parallel tree,
checkpoint v2).
**Modules:** M3.1 → M3.6, one per response, in order.

---

## M3.1 — B&C tree (`src/bnc/tree.{hpp,cpp}`)

- `struct BnCNode { uint32_t id, depth; double dual_bound; BoundChanges bounds;
  BasisHandle parent_basis; NodeState state; };`
- Node processing: restore parent bounds (bound changes list), warm-start dual simplex from
  the parent basis (bound changes are handled natively by the dual simplex — verify, do not
  assume: debug assert that the restored basis stays dual-feasible after bound changes).
- Integral-and-connected LP solution => extract tour (M3.4), update global UB (atomic).
- Node selection: **best-bound with an initial dive** (depth-first until first integral
  solution, then best-bound), CLI-switchable.
- Global loop honors `--time-limit` (anytime: on timeout, finish in-flight node, checkpoint,
  emit `FINISHED` with best UB/LB/gap).

## M3.2 — Branching (`src/bnc/branching.{hpp,cpp}`)

- **Reliability branching:** while a variable has < `reliability` (default 4) pseudocost
  observations, use **strong branching**: simulate up to 10 dual pivots on each child LP
  (x_e -> 0 and x_e -> 1 bound changes) for the top-K candidates by `|x_e - 0.5|`
  (K = 8; per-node strong-branching budget: 60 simulated pivots). Afterwards, pseudocost-only.
- Candidate pre-filter: only fractional variables (`x_e > 1e-6` and `x_e < 1 - 1e-6`); prefer
  edges with high `x_e` variance history.
- Tie-breaking deterministic (lowest edge index) — required for reproducibility.
- Tests: `T_branching_deterministic` (same seed + threads=1 => identical tree node order),
  `T_strong_branching_budget` (never exceeds per-node budget).

## M3.3 — Reduced-cost fixing (`src/bnc/rcf.{hpp,cpp}`)

- Implement MASTER §5.5 exactly, with the two scoping levels:
  - **Root level:** fixes with the root global bound (fully priced or Farley) are permanent; update edge `status` flags and
    log counts (`pruned_edges_rcf`).
  - **Node level:** fixes with the node's globally valid bound are stored in the node's `BoundChanges`
    and valid only in the subtree; they are applied/restored with the bound-change list.
- Both rules (`x_e = 0` and `x_e = 1` fixing) implemented with the sound rounding of MASTER §5.5:
  `ceil(z + |c_e_bar| - 1e-6) >= UB`.
- Re-run RCF after every UB improvement (queue a cheap pass at the next node boundary).
- Tests: **`T_rcf_never_prunes_optimal_edge`** — for >= 20 small instances (n <= 11, brute-force
  optimal tour known), run the root LP + RCF with UB = optimal and assert no optimal-tour edge
  is pruned; also run with UB = optimal + 100 and verify the pruning set grows (sanity).

## M3.4 — Tour extraction (`src/bnc/tree.cpp` helper or `src/graph/tours.hpp`)

- From an integral, degree-feasible, connected LP solution: walk the cycle, build `Tour`,
  recompute cost with `euc2d` (never trust LP sums for the reported UB), update UB, write
  `.tour` on every improvement (atomic write).

## M3.5 — Verification pricing (`src/bnc/pricing.hpp`, MASTER §5.6)

- On apparent optimality (tree empty or LB = UB): compute reduced costs of every non-candidate
  edge via the spatial grid + final duals (include cut duals via per-node cut membership);
  SIMD path with scalar fallback; parity-tested.
- If any `c_e_bar < -1e-6`: insert columns (status `0x10`), re-optimize the affected node,
  continue the tree. Loop until clean; only then emit `status: "OPTIMAL_PROVEN"` with
  certificate summary (final LB, UB, verification rounds).
- Tests: `T_verification_pricing_planted_edge` (remove a known optimal-tour edge from the
  candidate graph of eil51; the solver must recover it via pricing and still prove optimality),
  `T_pricing_simd_parity`.

## M3.6 — Checkpoint v2 + parallel tree

- Checkpoint v2: node frontier (bounds, bound changes, parent chain compaction), global UB/LB,
  cut pool, pseudocosts, RNG state, counters. Atomic multi-file write with manifest; `--resume`
  must reproduce the direct run **with the same seed** (`T_checkpoint_resume_equivalence`).
- Parallel tree (TBB): node tasks; global LB as atomic max; cut pool concurrent (M2.5);
  deterministic mode `--deterministic` fixes processing order (single-threaded order preserved
  via ordered queue) — document the throughput cost.
- Optional root-parallel separation already exists from Sprint 2; keep node LPs single-threaded
  by default; `--parallel-lps N` enables concurrent node LPs **bounded by the memory budget**
  (full memory-aware scheduler lands in Sprint 4; here, cap N = 2 and assert peak RAM <= 2 GB).

---

## SPRINT 3 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G3.1 | Band A: optimality **proven** (LB = UB integral + verification pricing clean) | eil51, berlin52, a280 in <= 1 min each; pr1002 <= 30 min; fl3795 <= 60 min |
| G3.2 | Certificate integrity | reported UB equals recomputed `euc2d` tour cost; LB_int == UB at proof |
| G3.3 | RCF property test | `T_rcf_never_prunes_optimal_edge` green (>= 20 instances) |
| G3.4 | Pricing recovery test | `T_verification_pricing_planted_edge` green |
| G3.5 | Checkpoint equivalence | `T_checkpoint_resume_equivalence` green (berlin52, mid-tree kill) |
| G3.6 | Tree node throughput | report nodes/s and LP time share on pr1002 (context for Sprint 4) |
| G3.7 | ASan + UBSan clean; deterministic mode reproducible | 0 findings; identical tours across 2 runs |

**Deliverables:** file tree diff, full code, optimality-proof transcripts (per instance: nodes,
cuts, time, certificate), README "Sprint 3 results (hardware: ...)".
