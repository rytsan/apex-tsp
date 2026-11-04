# SPRINT 1 — NATIVE LP ENGINE (bounded dual simplex from scratch)

> Paste **00_MASTER_PROMPT.md** together with this file.
**Existing (stable):** Sprint 0 modules (reader, Hilbert, grid, Delaunay, candidate graph,
2-opt, CLI, checkpoint) — treat their public APIs as frozen.
**Builds:** Layer 3 (native LP) + Netlib validation harness.
**Modules:** M1.1 → M1.6, one per response, in order.

**Scope guard:** cut separation does NOT exist yet. This sprint validates the LP engine on
Netlib LPs and on a TSP-shaped stress LP. The pr1002 subtour-LP sanity check (Band E2) happens
in Sprint 2, once separators exist.

---

## M1.1 — CSC matrix with row append (`src/lp/csc.{hpp,cpp}`)

- `class CscMatrix` — `add_column(j, entries)`, bulk build from triplets, `transpose()`
  (internal CSR view, built once and kept in sync), `column(j)` span access,
  `append_rows(const std::vector<Row>&)` — new rows go to a **row-wise buffer** that is merged
  into the CSC structure when the buffer exceeds `max_buffered_rows` (default 4096).
- Row semantics: constraints `row * x (>=|<=|=) rhs`; store `sense` and `rhs` in a parallel
  `RowData` vector (degree rows are `= 1`; SEC/blossom rows are `>=`).
- All coefficients in **double**. Matrix entries for degree/incidence/cuts are `+1.0`.
- Tests: `T_csc_transpose_idempotent`, `T_csc_append_rows` (solution of a small LP unchanged
  before/after append), `T_csc_column_view`.

## M1.2 — Sparse LU with Markowitz threshold pivoting (`src/lp/lu.{hpp,cpp}`)

- `class LuFactorization` — factorize a square basis matrix B (given as column list) with
  Markowitz count-minimizing pivot search and **threshold stability** (default 0.01).
- `ftran(a)` (solve Bx = a) and `btran(a)` (solve B^T x = a) via triangular solves over the
  sparse L/U factors (pivot order array + sparse triangular storage).
- Expose `fill_in()`, `density()` for the refactorization scheduler.
- Tests: `T_lu_residual` (`||Bx-b||/||b|| <= 1e-8` on 50 random sparse bases up to 5000x5000
  with ultra-sparse columns — 1–3 nnz per column, mimicking TSP degree bases),
  `T_lu_markowitz_threshold` (pivot magnitude >= threshold * column max at selection time).

## M1.3 — PFI eta updates (MVP) (`src/lp/eta.{hpp,cpp}`)

- `class EtaFile` — product-form updates: after a basis change with entering column
  `aq = B^-1 a_q` and pivot row r, append an eta vector; `ftran/btran` apply the eta file
  before/after the base LU factors.
- **Dynamic refactorization scheduler:** refactorize when `iterations_since_refact >= 100`
  OR `eta_fill_in >= 2 * lu_fill_in_at_last_refact`, whichever first (the old fixed-64 rule is
  deliberately dropped).
- Tests: `T_eta_accumulated_residual` (residual <= 1e-6 after 64 updates on random bases),
  `T_eta_refact_schedule` (scheduler fires on both triggers).

## M1.4 — Bounded dual simplex (`src/lp/dual_simplex.{hpp,cpp}`)

```cpp
struct SimplexOptions {
    enum class Pricing { Devex, DualSteepestEdge } pricing = Pricing::Devex;
    double feasibility_tol = 1e-7;   // 1e-9 in final-proof mode
    double zero_tol = 1e-11;
    double pivot_tol = 1e-5;
    double harris_delta = 1e-7;
    bool   bound_flipping = true;    // BFRT
    uint32_t refactor_interval = 100; // dynamic, see M1.3
    uint64_t iteration_limit = UINT64_MAX;
    double time_limit_s = INFINITY;
};
struct SimplexResult {
    enum class Status { Optimal, IterationLimit, TimeLimit, Unbounded, Infeasible, StallRecovered } status;
    double objective;             // primal value of the LP
    std::vector<double> duals;    // row duals (y)
    std::vector<double> reduced_costs;
    std::vector<uint8_t> var_state; // per column: basic / at_lb / at_ub / fixed
    uint64_t iterations, refactorizations; double solve_time_s;
};
SimplexResult solve_dual(const Lp&, const SimplexOptions&);
```

- **LP form:** minimize `c^T x` with bounded variables `l <= x <= u` (no explicit bound rows);
  rows in the CSC matrix with senses/rhs as in M1.1.
- **Initial basis:** all-slack, **dual-feasible** (all structural variables at lower bound when
  `c >= 0` => reduced costs >= 0). No dual phase-1 in the happy path. A simple **primal**
  phase-1 (composite objective on infeasibilities) must exist for robustness after bound
  changes — required, tested, but expected to trigger rarely.
- **Pricing:** dual Devex (default) with reference framework; `DualSteepestEdge` option with
  reference weights + periodic renormalization.
- **Ratio test:** Harris two-pass (delta from `harris_delta`, larger accepted step in pass 1,
  largest-pivot selection among acceptable candidates in pass 2) **+ BFRT** (flip bounded
  nonbasic variables whose ratio <= accepted step instead of blocking, to lengthen dual steps).
- **Bound flips** are recorded in `var_state` (at_lb <-> at_ub) without matrix changes.
- **Stall recovery:** if no dual-bound progress for `stall_window` iterations (default 500),
  apply cost perturbation (deterministic, seeded), resolve, then remove perturbation and
  re-optimize; report `StallRecovered`.
- **Warm start:** `solve_dual` accepts an optional starting basis + var_state; adding `>=` rows
  with zero-cost slacks must preserve dual feasibility (assert it in debug builds).
- Tests: `T_harris_no_cycling` (degenerate LP from a known degenerate family, 10k iterations,
  no cycle), `T_bfrt_long_steps` (LP where BFRT strictly reduces iteration count vs. BFRT off —
  verify on a constructed example), `T_dual_warmstart_after_rows`, `T_stall_recovery_triggers`.

## M1.5 — MPS reader (`src/lp/mps.{hpp,cpp}`)

- Read fixed/free MPS (ROWS, COLUMNS, RHS, RANGES, BOUNDS sections) sufficient for the Netlib
  files: `adlittle`, `25fv47`, `sc205`, `share2b`, `grow22` (https://www.netlib.org/lp/data/).
- Tests: `T_mps_reader_matches_names` (row/column counts vs. netlib index values — fetch and
  cite the URL per MASTER C0.4; if unreachable, mark "value not verified").

## M1.6 — Benchmark harness (`tools/lp_bench.cpp`)

- Runs the 5 Netlib LPs + a **TSP-shaped stress LP**: pr1002 candidate graph (from Sprint 0)
  with 2n degree rows + 5000 random `>= 2` rows over random node subsets; reports per-LP:
  objective, iterations, refactorizations, time, peak RAM.
- Comparison procedure against HiGHS **in the test environment only** (documented in
  `docs/LP_GATE.md`; HiGHS is never linked into the product): run the `highs` CLI on the same
  files, record time and iterations, paste both tables.

---

## SPRINT 1 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G1.1 | Netlib objectives match published optima | within 1e-6 relative, 5/5 (values fetched + URLs cited) |
| G1.2 | Performance gate (Band E1) vs. HiGHS on the same files | total time <= 10x, iterations <= 3x |
| G1.3 | Stress LP (pr1002-shaped, ~2000 rows x ~4000 cols) | solves to optimality; report iterations/s |
| G1.4 | §8.2 tests for this sprint (`T_csc_*`, `T_lu_*`, `T_eta_*`, `T_harris_*`, `T_bfrt_*`, `T_dual_*`, `T_stall_*`, `T_mps_*`) | 100% green, output pasted |
| G1.5 | ASan + UBSan clean on the full test suite | 0 findings |

**Decision point:** if G1.2 fails, STOP and invoke MASTER §8.3 Plan B planning (Held-Karp
Lagrangian becomes the primary LB; redesign Sprints 2–3 around it) — do not silently continue.

**Deliverables:** file tree diff, full code, `docs/LP_GATE.md` with the HiGHS comparison
transcript, README "Sprint 1 results (hardware: ...)".
