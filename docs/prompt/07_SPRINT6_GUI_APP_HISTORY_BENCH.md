# SPRINT 6 — COMPLETE GUI APP, HISTORICAL BENCHMARK PIPELINE

> Paste **00_MASTER_PROMPT.md** together with this file. Operator-requested sprint (2026-11-08), not in
> the original kit; recorded in README §9.
**Existing (stable):** Sprints 0–5 (engine, IPC protocol v1 + server, Avalonia/SkiaSharp monitor GUI,
campaign tooling, `docs/REPORT.md`).
**Builds:** (1) the GUI becomes a complete desktop app in the spirit of the Concorde GUI, with a modern
look: project/run management inside the window, live view of the solver's computations (LP solution,
cuts, B&C tree, heuristic vs. LP vs. Lagrangian provenance by color, gap chart), heuristic lab and
instance editor, benchmark tab. (2) A historical benchmark pipeline: ~40 TSPLIB instances against their
known optima, the published history of their solution, and Concorde / LKH-3 run on the same machine,
with a comparison table and report.
**Modules:** M6.1 → M6.7, one per response, in order.

Hard rules kept from MASTER: no new runtime dependency in the engine; Concorde, QSopt and LKH-3 live in
the test environment only (built by script, never linked, never redistributed, licences respected);
every bound shown in the GUI or report carries its source; mona-lisa100K is never called optimal;
numbers without thousands separators.

---

## M6.1 — Protocol v2: typed solver events (`src/ipc/`, `gui/ApexTsp.Ipc`)

New message types (version 2; v1 clients keep working through Hello negotiation, unknown types skipped):

| Type | Payload | Rate |
|---|---|---|
| `LpSolution` 0x25 | u64 epoch, u32 count, count x {u32 u, u32 v, f32 x} (support edges, x > 1e-6) | coalesced, <= 2 Hz, cap 2^20 edges |
| `CutRound` 0x26 | round, node id, lp value, rows, per-family found / added (SEC, blossom, comb, component, pool), max violation, lp s, sep s | every round, FIFO |
| `TreeNode` 0x27 | id, parent, depth, dual bound, state (open/processing/pruned/infeasible/integral/branched), branch edge + dir | every change, FIFO |
| `Event` 0x28 | typed milestone: ub_improved {value, source: construct/2opt/oropt/lk/ils/dive/lp_guided/lp_integral/decomp}, lb_improved {value, source: lp/lagrangian}, phase {name}, no_violated_cuts, tailing_off, rcf {fixed0, fixed1} | FIFO |
| `Command` 0x15 | GUI -> engine: run heuristic {name, params} on the current instance / tour | ack'd |

- Engine: emitted from the existing hooks (cut loop, tree conclude, primal offers, HK); no solver
  semantics change; emission cost measured (target <= 1% wall on pr1002 300 s with GUI attached).
- C# client mirrors the structs; sizes checked against the header as in M5.3.
- Tests: `T_v2_roundtrip_all_types`, `T_v1_client_ignores_v2`, `T_lp_solution_cap_and_coalesce`,
  `T_tree_events_match_bnc_result` (node count / pruned / branched equal the B&C totals).

## M6.2 — App shell, projects and runs (`gui/ApexTsp.Gui`)

- Modern look: Fluent theme, dark/light (follows OS, switchable), one accent palette; tabbed layout
  **Solve | Heuristics lab | Editor | Benchmarks | Log**; dockable side panels; keyboard shortcuts.
- Open `.tsp` (dialog, drag-and-drop, recent list); instance summary (n, metric, bbox, known optimum
  if in the reference table). Run settings in the window: mode, time limit, threads, seed, memory
  ceiling, checkpoint dir; Start / Pause / Resume / Cancel / Checkpoint; resume from checkpoint.
- Save tour (`.tour` TSPLIB), save instance, export map PNG/SVG, export run report JSON.
- Run history per instance (app data dir, JSON): table of past runs, click to reload tour + charts.
- Tests: view-model tests for settings to engine arguments, history persistence round trip.

## M6.3 — Live solver view (`gui/ApexTsp.Render`, `gui/ApexTsp.Gui`)

What a solver user sees while it works:
- **Provenance colors** everywhere (legend fixed): heuristic UB = orange, LP bound = blue,
  Lagrangian (HK) bound = purple, proven = green. Status badge: "HEURISTIC", "LP BOUND",
  "LAGRANGIAN BOUND", "OPTIMAL PROVEN", "CANCELLED".
- **Map overlays:** current tour; LP support graph (x = 1 solid, 0 < x < 1 width/alpha by x, color
  scale legend); toggle each layer; cut highlight (nodes of the last violated SEC / blossom handle
  flash briefly); clusters (Sprint 5).
- **Gap chart:** UB and LB over time with markers per `Event` (shape = source), log/linear axis,
  gap % curve; hover shows value and source.
- **Cuts panel:** per-round table + stacked bar chart by family; clear "no violated cuts found" /
  "tailing off" states; totals; LP value per round line.
- **B&C tree panel:** live tree drawing (depth layout, color by state, label by bound), node count,
  open / pruned / max depth; click a node to show its bound and branch.
- **Log tab:** structured, filterable, color by category (heuristic / LP / cuts / tree / system).
- Performance: all overlays inside the 16 ms frame budget at 100k nodes (LP overlay decimated with
  the same LOD rules); measured.

## M6.4 — Heuristics lab and instance editor

- Lab: buttons Nearest neighbor, Greedy edge, Space-filling curve, 2-opt, Or-opt, LK (Or-3opt),
  ILS (time box), Decomposition; each runs in the engine through `Command`, animated (tour updates
  streamed), length and delta per step; chain steps; compare two tours.
- Editor: click to add / drag / delete points, random generation (uniform, clustered, grid), metric
  choice (EUC_2D, CEIL_2D, ATT, GEO), node labels and hover (id, coords, tour neighbors, edge length).
- Engine side: `--mode heuristic --heuristic <name>` and the `Command` path reuse existing primal
  code only.
- Tests: each heuristic via `Command` returns a valid permutation; editor save -> engine load round trip.

## M6.5 — Historical reference data (`tools/history/`)

- `tools/history/fetch_tsplib.py`: downloads the instance set (TSPLIB symmetric, <= ~20k nodes, plus
  the historical milestones and pla85900 / mona-lisa100K already in `data/`), verifies sha256 against
  a committed manifest; instances stay out of git (`data/tsplib/`, gitignored).
- `tools/history/reference.csv` (committed): instance, n, optimum or best known (flag), and the
  history of its solution: year first solved, team, method, published time + hardware, **source URL
  for every field**. Sources: TSPLIB optimal values list, the Waterloo TSP site (milestones,
  Concorde results), Applegate-Bixby-Chvátal-Cook (2006). Unknown field = empty, never guessed.
- Instance set (~40): every symmetric TSPLIB instance up to ~3000 nodes that the exact mode can
  attempt, plus the milestones dantzig42, gr120, lin318, att532, gr666, pr2392, plus usa13509,
  d15112, d18512 (scalable), pla85900, mona-lisa100K. Final list in the module report.
- Test: CSV schema check (every row has a source; optimum matches the instance file's comment where
  present).

## M6.6 — Same-machine baselines (`tools/baselines/`, test environment only)

- Scripts that download and build Concorde (with QSopt; academic licence) and LKH-3 into
  `third_party_test/` (gitignored), never linked to the product, with a version + checksum record.
- Runners producing the same per-run JSON schema as `run_benchmarks.sh` (status, ub, lb, wall,
  peak RAM, threads = 1 for Concorde), so the report can put them side by side.
- If a build fails on this machine: documented in README §9, the report marks the column "not run".

## M6.7 — Historical campaign + comparison report (`tools/run_history.sh`, `docs/HISTORY_REPORT.md`)

- Matrix: every instance x {APEX exact (time box by size), APEX scalable, Concorde, LKH-3};
  per-run JSON; resumable (skips finished runs); one run at a time.
- `make_history_report.py` -> `docs/HISTORY_REPORT.md` + SVG charts:
  - per-instance table: n, known optimum (source), year solved / team, published time (hardware),
    APEX status / UB / LB / certified gap / time, Concorde status / time, LKH-3 tour / time,
    APEX vs Concorde time ratio;
  - summary: instances proven by APEX, median ratio vs Concorde, gap distribution, time vs n chart;
  - historical narrative table: milestone instances, then vs now (same-machine time).
- GUI Benchmarks tab reads the same JSON: sortable table, charts, click a row to open the instance
  and its tour.

---

## SPRINT 6 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G6.1 | Protocol v2 | M6.1 tests green; v1 client still works; telemetry overhead <= 1% wall on pr1002 300 s |
| G6.2 | App workflow | open instance -> configure -> run -> pause/resume -> save tour -> reload from history, done in the app (screenshots + log) |
| G6.3 | Solver view | live pr1002 exact run: LP overlay, cuts panel, tree panel, provenance colors, gap chart; fl3795 + pla85900 frame-time p95 <= 16 ms with all overlays |
| G6.4 | Lab + editor | every heuristic via the app returns a valid tour; edited instance solves to a proven optimum |
| G6.5 | Reference data | `reference.csv` every row sourced; manifest checksums verified |
| G6.6 | Baselines | Concorde and LKH-3 run on this machine, or failure documented with the build log |
| G6.7 | History report | `docs/HISTORY_REPORT.md` generated for the full set; GUI Benchmarks tab shows it |
| G6.8 | Hygiene | engine suite + ASan/UBSan/TSan clean; GUI tests green; NativeAOT publish succeeds; Band A regression holds |

**Deliverables:** file tree diff, full code, `docs/HISTORY_REPORT.md`, README §9 entries and a Sprint 6
results section.
