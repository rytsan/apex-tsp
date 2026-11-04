# SPRINT 0 — VERTICAL SLICE (de-risk early, end-to-end value)

> Paste **00_MASTER_PROMPT.md** together with this file. Nothing exists yet: this sprint creates
> the repository skeleton and the first end-to-end path. All later sprints assume this sprint's
> APIs as stable.

**Existing (stable):** nothing — you are bootstrapping.
**Builds:** Layers 0 and 1 + first primal engine + CLI + CI.
**Modules:** M0.1 → M0.8, one per response, in order.

---

## M0.1 — Repository skeleton + TSPLIB reader (`src/core/instance.{hpp,cpp}`)

- Initialize `apex-tsp/` exactly with the layout in MASTER §3 (empty files allowed only where
  later modules will fill them; every delivered file must compile).
- CMake >= 3.28: `CMakePresets.json` with `dev` (GCC/Clang/MSVC), `benchmark`, `asan` presets;
  `engine` + `engine_tests` targets; `.github/workflows/ci.yml` (build + tests on Linux/GCC and
  Windows/MSVC).
- `struct Instance { std::string name; uint32_t n; std::vector<double> xs, ys;
  std::vector<uint32_t> original_ids; };`
- `Instance read_tsplib(const std::string& path);` — parse `NAME`, `TYPE`, `DIMENSION`,
  `EDGE_WEIGHT_TYPE`, `NODE_COORD_SECTION`, `EOF`. Accept `TSP` + `EUC_2D`/`CEIL_2D` only (MASTER §2.1 exception); every other
  combination is a hard error with a clear message (no silent wrong answers). Accept `NODE_COORD_SECTION`; reject an instance that only has
  `DISPLAY_DATA_SECTION` (no usable coordinates for EUC_2D) with a clear error; tolerate CRLF and arbitrary
  whitespace.
- Must load **all** MASTER §7 instances without error (add a parameterized test that loads each
  one and checks `DIMENSION` against the table).
- Tests: `T_tsplib_roundtrip_minimal` (write a tiny in-memory instance, read it back),
  `T_tsplib_rejects_non_euc2d`.

## M0.2 — Hilbert ordering (`src/geo/hilbert.{hpp,cpp}`)

- `uint32_t hilbert_key(double x, double y, double min_x, double min_y, double max_x, double max_y);`
  — map original coordinates onto the 2^16 x 2^16 grid (scaling for indexing only; **distances
  never use scaled coordinates**, MASTER §2, directive 1) and compute the Hilbert index.
- `std::vector<uint32_t> hilbert_order(const Instance&);` — indices sorted by key.
- Tests: `T_hilbert_roundtrip` (key -> (u,v) -> key, 10^6 random points),
  `T_hilbert_locality` (mean pairwise grid distance of adjacent tour entries << global mean).

## M0.3 — Spatial grid (`src/geo/grid.{hpp,cpp}`)

- `class SpatialGrid` — uniform grid over the instance bounding box; cell lists;
  `knn(i, k, out)`; `for_each_in_radius(i, r, fn)`. Build O(n), k-NN query O(cell occupancy).
- Used by: k-NN graph, candidate fallback, verification pricing (MASTER §5.6).

## M0.4 — Delaunay triangulation (`src/geo/delaunay.{hpp,cpp}`)

- Own implementation, O(n log n), sweep-hull or divide-and-conquer. Output: edge list
  (undirected, deduplicated).
- Tests: `T_delaunay_empty_circle` (for random point sets n <= 200, verify the empty-circle
  property by brute force on sampled triangles), `T_delaunay_edge_count`
  (m in [2n-5-h, 3n-3-h] range for planar point sets).

## M0.5 — Candidate graph (`src/graph/candidate_graph.{hpp,cpp}`)

- `struct CandidateGraph { uint32_t n; std::vector<Edge> edges; CSR adjacency; };`
- Build: union of Delaunay + symmetric k-NN (k = 8 default, `--knn`), dedup by canonical pair,
  `cost` via `euc2d` (MASTER §5.3), `status = 0x04`.
- Connectivity check; if disconnected, add MST edges until connected (log how many).
- Tests: `T_candidate_graph_connected` (all §7 instances), `T_candidate_graph_symmetry`
  (adjacency symmetric, no self-loops, no duplicate pairs).

## M0.6 — Primal engine v1 (`src/primal/two_opt.{hpp,cpp}`)

- `struct Tour { std::vector<uint32_t> order; int64_t cost; };`
  `int64_t tour_cost(const Instance&, const Tour&)` (integer, EUC_2D).
- `Tour hilbert_tour(const Instance&, const std::vector<uint32_t>& hilbert_order);` (UB0).
- `void two_opt_oropt(Tour&, const Instance&, const CandidateGraph&, const TwoOptConfig&);`
  — 2-opt + Or-opt (segments of 1–3) with **don't-look bits** and neighbor lists from the
  candidate graph (size adaptive: start 10, grow while improvements found). Deterministic given
  `--seed`.
- Tests: `T_two_opt_improves_hilbert` (gap strictly decreases on eil51/berlin52/a280),
  `T_two_opt_deterministic` (same seed => same tour).

## M0.7 — Tour I/O + report (`src/core/tour_io.{hpp,cpp}`, report stub)

- Write/read TSPLIB `.tour` format; reader validates the permutation.
- `--report <json>`: emit `{instance, mode, ub, lb: null, gap_pct: null, wall_s, seed, threads,
  ram_peak_bytes}` (lb/gap fields present but null until Sprints 1–3 provide real values).

## M0.8 — CLI + telemetry stub + checkpoint stub

- `src/app/main.cpp` + `src/app/cli.hpp`: full MASTER §3 CLI (flags accepted; `--mode scalable`
  currently behaves as "primal only"; `--mode exact` returns a clear "not implemented until
  Sprint 3" error).
- Structured logs (`key=value` lines, single line per event, flush-safe) and a
  `TelemetrySink` interface (log-backed now; pipe-backed in Sprint 5).
- `src/bnc/checkpoint.{hpp,cpp}`: write/read a v1 checkpoint (instance hash, best tour, config,
  RNG state, wall clock) with atomic write (temp + rename). `--resume` restores it.
- Tests: `T_checkpoint_roundtrip`, `T_cli_rejects_unknown_flags`.

---

## SPRINT 0 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G0.1 | All MASTER §7 instances load; DIMENSION matches the table | 10/10 |
| G0.2 | mona-lisa100K: Hilbert + 2-opt/Or-opt end-to-end | <= 15 min, gap <= 15% vs. 5757191, peak RAM <= 1 GB |
| G0.3 | Same command on pla85900 and usa13509 | completes, no crash, tour file valid |
| G0.4 | §8.2 tests implemented so far (`T_tsplib_*`, `T_hilbert_*`, `T_delaunay_*`, `T_candidate_*`, `T_two_opt_*`, `T_checkpoint_*`) | 100% green, output pasted |
| G0.5 | CI workflow green on Linux + Windows | badges pass |

**Deliverables:** file tree diff, full code per module, `CMakePresets.json`, CI yaml,
benchmark transcript (command + timing + RAM + gap) for G0.2–G0.3, README section "Sprint 0
results (hardware: ...)".
