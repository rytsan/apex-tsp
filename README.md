# APEX-TSP Prompt Kit — Agent Operating Instructions

[![ci](https://github.com/rytsan/apex-tsp/actions/workflows/ci.yml/badge.svg)](https://github.com/rytsan/apex-tsp/actions/workflows/ci.yml)

> **You are an AI agent.** If you were given this kit, this file is your operating manual.
> Kit files live in `docs/prompt/`. Read it fully together with `00_MASTER_PROMPT.md` and exactly one sprint file (`01`–`06`)
> before writing any code. The human operator should read §6 once.
> Rules in `00_MASTER_PROMPT.md` (contract C0.1–C0.8, directives §2, spec §5–§8) always win.
> This README only tells you **how to operate** under those rules.

---

## 1. Kit map

All kit files below are in `docs/prompt/` (except this README, at the repo root).

| File | Role | Pasted together with |
|---|---|---|
| `00_MASTER_PROMPT.md` | Shared binding spec: execution contract, modes, metric, structs, RCF rules, reference data | every session |
| `01_SPRINT0_VERTICAL_SLICE.md` | Bootstrap: reader → Hilbert → candidate graph → 2-opt → CLI/CI | session 1 |
| `02_SPRINT1_NATIVE_LP_ENGINE.md` | Native bounded dual simplex + Netlib harness | after G0 passes |
| `03_SPRINT2_CUT_ENGINE.md` | SEC + blossom separators, cut pool, root cut-and-resolve loop | after G1 passes |
| `04_SPRINT3_BRANCH_AND_CUT.md` | B&C tree, branching, RCF, verification pricing, optimality proofs | after G2 passes |
| `05_SPRINT4_PRIMAL_PARALLEL_MEMORY.md` | LK-lite/ILS, decomposition + patching, Held-Karp, memory enforcement, Forrest-Tomlin | after G3 passes |
| `06_SPRINT5_GUI_IPC_BENCHMARKS.md` | IPC protocol, Avalonia/SkiaSharp GUI, final benchmark campaign | after G4 passes |
| `README.md` | This operating manual | — |

**Sequence is strict:** sprints in order, gates between sprints, modules in order inside each
sprint. Module IDs are `M<sprint>.<n>` (e.g., `M2.3`); gate IDs are `G<sprint>.<n>`.

---

## 2. Session bootstrap (every new session)

1. **Inventory check.** Confirm you received the master + exactly **one** sprint file. If two
   sprint files arrived, ask which one is active before doing anything (C0.8).
2. **Read everything first** — master and sprint file, completely — before writing code.
   Skimming is a spec violation.
3. **Post a Sprint Acknowledgment** (≤ 200 words) at the top of your first response:
   - sprint goal in one sentence;
   - module list with order (`M#.1 → M#.n`);
   - the gate IDs that close the sprint;
   - any blocking questions (if you have none, write "no blocking questions").
4. **Then start M#.1 in the same response.** Do not wait for an extra go-ahead unless the
   operator asked for one.

---

## 3. Module loop (core protocol)

**One module per response.** Use this response template, exactly these headings:

```
## Module M#.# — <name>
### Files changed
<full paths, created/modified, one per line>
### Code
<complete code; no placeholders, no "..." (C0.2)>
### Build & run
<exact commands>
### Test output (real)
<pasted, summarized — only results you actually executed (C0.3)>
### Self-check (MASTER §9)
<itemized answers to the checklist, not "all ok">
```

End every module response with one line:
- `NEXT: M#.# — say APPROVED to continue`, or
- `SPRINT COMPLETE — run gates G#.*` (list them).

**Hard rules of the loop:**
- Never start the next module before the operator says **APPROVED**.
- Never batch two modules into one response. Never reorder modules.
- If the module does not fit: stop at a clean cut point and end with
  `[CONTINUES — say 'continue']`; when continuing, do not re-deliver code already shipped.
- If the sprint file's assumption about an earlier module is violated by the actual repo,
  STOP that module and report the mismatch (§5) — never silently patch a stable API.

---

## 4. Gates and failure handling

- After `SPRINT COMPLETE`, provide the **exact gate commands** (build profiles, test invocations,
  benchmark invocations with instance paths, limits, seeds) so the operator can run them, or run
  them yourself if you have shell access.
- Gate evidence = real output + hardware context (MASTER §3) + seed + threads + limits. A gate
  without evidence is not passed, no matter how confident you are.
- **Failure protocol:**
  1. Failure in code delivered this sprint → fix and re-run (up to **2 fix rounds**), re-pasting
     evidence. Say which gate criterion failed and what you changed.
  2. Still failing after 2 rounds → **STOP**. Post a root-cause analysis and 2–3 candidate
     remedies with trade-offs. Wait for the operator's decision.
  3. Failure that invalidates the plan (e.g., gate `G1.2` LP performance, which has an explicit
     decision point) → **STOP immediately**, invoke the documented decision point
     (e.g., MASTER §8.3 Plan B), propose the revised plan, wait for approval.
     **Silently continuing after a plan-invalidating gate failure is the single most serious
     protocol violation.**
- Gates may only be refined (targets adjusted) by explicit operator approval, recorded in the
  repo README — never by the agent alone.

---

## 5. Mid-project session recovery

If a session starts and the repo already contains earlier sprints (operator pastes master +
sprint N, but code exists):

1. **Rebuild + run the full test suite first.** Paste the summary. The repo, its checkpoints
   and `docs/REPORT.md` are the **only** source of truth — you have no memory of prior
   sessions.
2. Reconcile: state which modules/gates of the current sprint appear done vs. pending.
3. Continue from the **first pending module**. Do not rewrite stable APIs from earlier sprints;
   if something is genuinely broken, that is a §4 failure, not a refactoring invitation.
4. If `--resume` state exists (checkpoint), prefer validating it (`T_checkpoint_resume_equivalence`)
   over starting fresh.

---

## 6. Operator quick start (human, read once)

1. Create an empty project folder; download the instance files from the sources in MASTER §7
   (TSPLIB, Waterloo, Netlib) into `data/` before the session that needs them (`data/` is git-ignored).
2. One session per sprint: paste `00_MASTER_PROMPT.md` + the sprint file; reply **APPROVED**
   after each module response; when you see `SPRINT COMPLETE`, run the gate commands (or let
   the agent run them), then move to the next sprint file.
3. Before every APPROVED, spend 60 seconds checking: tests actually ran (real output, not
   claims), no leftover `TODO`, file tree matches MASTER §3 layout, self-check items answered
   individually.
4. Keep the repo (`git` recommended), checkpoints and `docs/REPORT.md` — they carry state
   across sessions.
5. You own the decisions: APPROVED, gate pass/fail, C0.7 alternative approvals, gate-target
   changes. The agent owns execution, verification and evidence.
6. Anti-patterns that should make you refuse APPROVED: code with placeholders; "tests pass"
   without pasted output; benchmark numbers without hardware context; any use of local bounds
   in pruning; any distance computed in float; exact-mode claims without verification pricing;
   silent changes to frozen APIs.

---

## 7. Hard-rule quick reference (agent cheat sheet)

| Rule | Where |
|---|---|
| Distances: EUC_2D, double, exact sqrt, original coordinates only | MASTER §2.1, §5.3 |
| Pruning: globally valid bounds only; local bounds = heuristic-marked, UB-only | MASTER §2.5, §5.5 |
| Modes: exact (≤ ~10k, proof + verification pricing) and scalable (100k+, gap) never mixed | MASTER §1, §5.6 |
| Memory: RAM-first; mmap for persistence/cold data only; ceiling enforced (Job Objects/setrlimit) | MASTER §2.3–2.4, §5.1 |
| Dependencies: no optimization libraries; infra (TBB/Avalonia/SkiaSharp/OS) allowed | MASTER §2.2 |
| Numbers: only MASTER §7 values; anything else fetched + cited or marked unverified | C0.4 |
| Evidence: paste real output or do not claim it | C0.3 |
| One module per response; APPROVED between modules | C0.1, §3 here |
| Exhaust all cut families before branching | MASTER §6 |
| mona-lisa100K = gap benchmark vs. 5757191, never "optimal" | MASTER §7 |

---

## 8. Glossary

- **APPROVED** — operator's command unlocking the next module. No other phrase counts.
- **Module (M#.#)** — one deliverable unit inside a sprint; one response each.
- **Gate (G#.#)** — measurable acceptance criterion closing a sprint; evidence required.
- **Band (A–E)** — performance/quality target families defined in MASTER §8.4.
- **Verification pricing** — reduced-cost scan of the complete graph before any optimality
  claim in exact mode (MASTER §5.6).
- **Heuristic marker** — code comment + telemetry flag identifying routines whose outputs are
  UB-only and must never feed pruning.
- **Plan B** — Held-Karp Lagrangian bound replacing the native LP as primary LB if the LP
  performance gate fails (MASTER §8.3).

---

## 9. Operator decisions log

Approved deviations from the original kit (README §4: refinements are recorded here).

| Date | Decision | Affected |
|---|---|---|
| 2026-11-04 | Reference values corrected per TSP-BEST.html: fl3795 = 28772, d18512 = 645238; numbers written without thousands separators | MASTER §7, all sprint gates |
| 2026-11-04 | `CEIL_2D` supported for pla85900 (`ceil2d`, same double/exact-sqrt rules) | MASTER §2.1, §5.3, M0.1 |
| 2026-11-04 | `DISPLAY_DATA_SECTION`-only instances rejected with a clear error | M0.1 |
| 2026-11-04 | Reference hardware lowered to >= 4 physical cores | MASTER §3 |
| 2026-11-04 | `.github/` at repo root (GitHub Actions requirement), not under `engine/` | MASTER §3 layout |
| 2026-11-04 | Root LP bound made globally valid: Farley bound with exact reduced costs over the complete graph; pricing adds negative columns every round (root LB reported only fully priced). Root RCF uses the sound rule ceil(global_lb + d - 1e-6) >= UB instead of LB_int + d > UB - eps (MASTER §5.5 formula can prune tours better than UB) | MASTER §5.5, M2.6 |
| 2026-11-04 | MASTER §5.5 rewritten to the sound RCF rule and the globally valid bound (fully priced LP or Farley); RCF eps removed | MASTER §5.5, §5.7, 04 M3.3 |
| 2026-11-04 | G2.1 / Band E2 upper limit changed from 99.5% to the optimum (stronger cuts than planned are not a defect; above the optimum stays a bug) | MASTER §7 Band E2, 03 G2.1 |
| 2026-11-05 | `T_rcf_never_prunes_optimal_edge` checks the exact sound-rule guarantee (cheapest tour through a 0-fixed edge >= UB) at UB = opt, opt + 1, opt + 100. With UB = opt an optimal-tour edge may legitimately be fixed (every tour through it costs exactly opt), and a larger UB fixes fewer edges, not more | 04 M3.3 |
| 2026-11-05 | Sprint 3 primal additions (operator-approved): LP-guided tour heuristic at nodes (greedy by x + 2-opt/Or-opt) and a dive depth limit before best-bound. They change only the UB and node order, never bounds or pruning | 04 M3.4, M3.1 |
| 2026-11-05 | Verification pricing (MASTER §5.6) runs at every node, not only at apparent optimality: every node bound is a Farley bound from exact complete-graph pricing (grid with an exact radius, AVX2 filter), so no node is ever pruned on a restricted-graph bound. `OPTIMAL_PROVEN` certificate = LB_int == UB, recomputed tour cost == UB, plus the count of verification pricing passes | 04 M3.5 |
| 2026-11-05 | Parallel tree with `std::thread` instead of TBB (operator-approved, no new dependency). Nodes run in deterministic batches of `--parallel-lps` (<= 2) workers on snapshots of the shared state, merged in worker order, so every parallel run is reproducible (`--deterministic` is the only mode); cost: each batch waits for its slowest node | 04 M3.6 |
| 2026-11-05 | `T_checkpoint_resume_equivalence` kills mid-tree on a280 and eil51: berlin52 (spec) is solved at the root, so it has no tree to interrupt | 04 M3.6, G3.5 |
| 2026-11-05 | G3.1 miss accepted (operator option 1): eil51 / berlin52 / a280 proven; pr1002 (gap 0.0117% at 1800 s) and fl3795 (gap 2.3905% at 3600 s) not proven. Their proofs move to Sprint 4 (performance) with the §13 numbers as the baseline. Gates unchanged | 04 G3.1, Sprint 4 |
| 2026-11-05 | Background ILS (M4.1) is opt-in for B&C (`root_bench --ils`, `BnCConfig::primal`): its UB arrives at timing-dependent node boundaries, so such runs are not bitwise reproducible (bounds and proofs unaffected); without it B&C stays deterministic. `apex_tsp` runs ILS in the foreground for the remaining `--time-limit` budget (no limit: stops after 2-opt as in Sprint 0). "Periodic kicks" = a 3-double-bridge kick after 1000 stalled iterations | 05 M4.1 |
| 2026-11-05 | LP diving (M4.2) fixes x_e > 1 - 1e-6 **and** the largest fractional x_e to 1 per round (fixing only the x_e ~ 1 columns leaves the LP unchanged), backtracks once (infeasible x_e = 1 becomes x_e = 0), and completes the deepest feasible dive solution with the LP-guided greedy when no Hamiltonian integral solution appears: the dive LP has no new SECs, so it turns infeasible or subtour-integral quickly (eil51: 0 integral tours in 20 dives without the completion). RIH-lite = x_e = 1 fragments + greedy reconnection + 2000 ILS iterations, once at the root. `ub_improved` telemetry carries `heuristic=true` for every non-LP source | 05 M4.2 |
| 2026-11-06 | Decomposition (M4.3): leaves in parallel with `std::thread` (same no-TBB decision as M3.6), results identical for any thread count. Patch stitch by exhaustive search over every tour edge at the 4 portal endpoints of each side (2 orientations) instead of a Hungarian/auction assignment: merging two cycles exchanges exactly two edges, so the enumeration is already optimal over the portal edges and an assignment solver adds nothing. KD split ratio floor(k/2):ceil(k/2) with k = ceil(size/leaf) gives leaves of size/k (pla85900: 795-796 at leaf 800). `apex_tsp --mode scalable` uses decomposition as the start tour when n > 2 x leaf size | 05 M4.3 |
| 2026-11-06 | Held-Karp (M4.4): the subgradient ascent uses Kruskal 1-trees on the candidate graph (fast, not a bound); the reported LB is the exact complete-graph minimum 1-tree at the best multipliers (dense Prim, O(n^2): 14 s on mona-lisa100K), so it is valid for every pi. Polyak step lambda (UB - L) / norm(g)^2, lambda0 = 2, halved after 30 stalled iterations. Plan B (Lagrangian exact mode) not activated: the Sprint 1 LP gate passed. Cross-check (c) in `root_bench --hk`: converged root LP below HK is reported as BUG. `T_hk_convergence` factor: HK >= 99.5% of the subtour LP on pr1002 (measured 99.988%). `apex_tsp --mode scalable` spends 10% of the remaining budget on the ascent | 05 M4.4 |
| 2026-11-06 | Memory (M4.5): Linux cap is `RLIMIT_AS` = limit + 1 GB (address space also holds thread stacks, malloc arena reservations and code; RSS is what the budget controls); not set in ASan/TSan builds (shadow memory needs terabytes of address space). Primary path = graceful stop: phase guards at 90% of the limit in `apex_tsp`, LP tokens in B&C = (limit - baseline) / LP working set, both from **deterministic size estimates** (graph, pool, frontier, LP nnz, peak LU nnz) so the schedule stays reproducible; RSS above the limit is only a safety net. `--parallel-lps` now 1..6. `T_mem_limit_graceful` uses a 20 MB ceiling instead of 256 MB: the ILS undo log was capped at n writes (mirror restore past that), which cut pla85900's scalable-mode peak from 593 MB to about 50 MB, so 256 MB no longer triggers MEM_LIMIT | 05 M4.5 |
| 2026-11-06 | LP updates (M4.6): Forrest-Tomlin is the default (`--basis-update=pfi` fallback); per C0.7 it would lose the default only by losing >= 10% on both benchmarks, and it wins fl3795 (LP 179 s vs 234 s, RSS 178 MB vs 334 MB) while losing 22% on the stress LP. Hypersparse FTRAN/BTRAN implemented (bitwise identical results, 0.4% of the entries touched on 3-nonzero right-hand sides) but off by default (`--hypersparse`): slower on both benchmarks (stress 0.99 s vs 0.94 s, fl3795 LP 179 s vs 173 s). FT update reuses the spike saved by the entering column's FTRAN. Pre-existing, not FT: on one random LP with free / half-bounded columns (`T_ft_equivalence` trial 35) both updates return the optimal objective but duals with KKT violation ~1; TSP LPs (0 <= x <= 1) are not affected | 05 M4.6 |
| 2026-11-07 | M4.7 (operator request, not in the kit): primal pipeline built around clustering + heuristics before the LP. Density-aware cuts (widest empty gap within the 35-65% ranks; leaves (0.35 x leaf, leaf], not 500-800), greedy-edge leaf start, merges of one level in parallel, each merge re-evaluated by ILS kicked at the stitch/boundary nodes, final polish as parallel tour segments with fixed endpoints (deterministic for any thread count). `apex_tsp --mode exact` now exists: heuristic warm-up (stops after 50 x n idle ILS iterations; one ILS per thread on small n), tour edges added to the candidate graph, B&C best-bound from the root with that UB, background ILS. Everything in this phase is UB only (`heuristic`) | 05 M4.7 |
| 2026-11-07 | Regression found and fixed while measuring M4.7: LP diving tours (M4.2) tighten node RCF, children become infeasible on the candidate graph and Farkas pricing added long columns no tour below the UB can use (pr1002, 300 s: 27610 priced columns vs 5938 in Sprint 3). Fix 1: global RCF pricing filter, a pair whose root reduced-cost lower bound c - Yroot_u - Yroot_v already gives ceil(root_lb + rc - 1e-6) >= UB never enters (sound rule, MASTER §5.5; same filter in the final node bound). Fix 2: diving off by default (`root_bench --dive`): with the filter it still lost (gap 0.467% vs 0.164% without) | 05 M4.2, M4.7 |
| 2026-11-04 | Root loop tailing-off stop (gain < 1e-4 relative over 50 rounds), reported separately from convergence | M2.6 |
| 2026-11-04 | Iterated SEC shrinking: contract a supernode pair only if both have x(delta) = 2 (merging across c > 1 hides violated SECs; found by `T_shrinking_safe`) | MASTER §6, M2.2 |
| 2026-11-04 | Blossom inequality corrected to the TSP 2-matching form `x(delta(H)\T) - x(T) >= 1 - |T|` (handle + odd teeth); the original `x(E(S)) <= (|S|-1)/2` is invalid for the TSP. Heuristic separation default, Padberg-Rao behind `--sep-blossom=full` | MASTER §6, M2.4 |
| 2026-11-04 | Dual phase 1 by artificial bounds (Koberstein), **only** when the initial basis is not dual feasible (generic LPs such as Netlib). TSP LPs (c >= 0, 0 <= x <= 1) never trigger it; asserted by tests | M1.4 |
| 2026-11-07 | G4.7 miss accepted (operator: continue, same path as G3.1): eil51 / berlin52 / a280 proven; pr1002 (gap 0.0157% at 1800 s) and fl3795 (gap 1.22% at 3600 s, Sprint 3: 2.39%) not proven. Cause: LB growth with SEC + blossom cuts only; stronger cuts (combs) are new scope. Gates unchanged; Band A in M5.4 reports actual values | 05 G4.7, 06 M5.4 |
| 2026-11-07 | Protocol (M5.1): frame `length` counts the bytes after the length field (4 header bytes + payload + 4 CRC bytes when `flags.0`). The decoder also checks the payload size per message type (fixed-size types exact, variable types capped: Log 64 KB, Close 4 KB) and resyncs byte by byte on any bad header or CRC: `T_framing_fuzz` showed that without the size check one random header claiming megabytes holds the decoder waiting. Message ids: Hello 0x01, Close 0x02, control 0x10-0x14 (payload u64 seq), Ack 0x1F, Telemetry 0x20 ... Finished 0x24; `TOUR_UPDATE` carries only {epoch, count}, the tour itself is in the SharedTour file. Seqlock uses release stores / acquire loads per word instead of fences (same code on x86; TSan does not support `atomic_thread_fence`). TSan binaries run under `setarch -R` (kernel ASLR layout) | 06 M5.1 |
| 2026-11-07 | Engine IPC server (M5.2). The kit says pause/cancel/checkpoint paths exist since Sprint 3/4; only the time limit and the periodic B&C checkpoint did. Added the minimum, no new solver semantics: `EngineControl` (process-wide flags); `pause_point()` sits next to the existing time-limit checks (simplex iterations, LK/ILS loops, B&C batch loop, Held-Karp iterations) and sleeps the thread with its state untouched (`T_control_pause_resume`: same tour as without a pause); cancel = the time limit expiring at those checks, report status `CANCELLED`; checkpoint = B&C saves at the next batch boundary + the app writes its v1 snapshot from the latest published tour. Not pausable in the middle: greedy construction, KD build, separation / pricing passes, the dense Prim 1-tree of HK (14 s at 100k); pause latency there is their length. Paused time counts toward `--time-limit`. The server logs the measured stall (`paused stalled_threads=N after_ms=X`). Report JSON (also the FINISHED payload) uses the Sprint 5 field order; `iterations` = simplex iterations (0 in scalable mode), `cuts_by_family` = {sec, blossom, component}, `hardware` = CPUID brand + logical cores. Windows pipe = PIPE_NOWAIT polling with 1 ms sleeps. `--shared-tour` default `<temp>/<endpoint name>.tour`. Cluster ids = KD-tree node index (leaves: solving/idle, merges: patching/idle). FINISHED waits up to 5 s for a client. TELEMETRY UB/LB/gap are NaN until known | 06 M5.2 |
| 2026-11-07 | GUI (M5.3): Avalonia 11.3.22 (kit: Avalonia 11; 12.x is current) with SkiaSharp 2.88.9 (the version Avalonia.Skia 11 ships), .NET 10. Render path = the documented CPU fallback for every platform: `TourRenderer` (SkiaSharp) draws into a `WriteableBitmap`, so one code path covers GPU-less machines and NativeAOT; the GPU lease path was not written. UI built in code (no XAML, no reflection bindings) so AOT needs no trimming exceptions. LOD: raw while <= 50000 nodes are on screen, then by on-screen density (nodes per pixel): decimated (one point per pixel, every k-th tour vertex) <= 0.25, clusters (6 px grid) <= 2, heatmap (12 px grid) above; strokes are hairlines (width-1 strokes cost 4x). The cluster heatmap overlay needs geometry that CLUSTER_MAP (`id, state`) does not carry: the engine logs one `cluster_box id x0 y0 x1 y1` event per KD-tree node (LOG frames, scalable mode only). `T_gui_struct_sizes_match_engine` reads the sizes, ids and versions from `engine/src/ipc/protocol.hpp` itself (generated check). Input latency = scripted input through the pointer handlers until the frame is rendered into the bitmap (compositor present not included). NativeAOT: the local SDK 10.0.401 bundles ILCompiler 10.0.12 with advisory GHSA-p3jp-9v83-76w9, which fails restore under warnings-as-errors; not suppressed: publish with SDK 10.0.403 (runtime 10.0.14) succeeds with zero AOT/trim warnings. WSL: the GUI needs `DISPLAY=:0` (WSLg) | 06 M5.3 |
| 2026-11-08 | Benchmark campaign (M5.4): `engine/tools/run_benchmarks.sh` + `make_report.py` write `docs/REPORT.md` from per-run JSON (`out/campaign/final`, not committed). Settings: seed 1, 5 threads (Band D: 1), `--mem-limit-mb 4096`, one run at a time. Band C runs 600 s per instance (the band allows 8 h; both instances already pass at 600 s). Band A small instances capped at 60 s. LKH-3 / Concorde not installed: no context rows | 06 M5.4, MASTER §7 |
| 2026-11-08 | Band B (usa13509 exact, stretch) deferred: not run. fl3795 (3795 nodes) is not proven in 60 min, so a 4 h usa13509 run would report a gap, not a proof; the budget was not spent | MASTER §7 Band B, 06 G5.4 |
| 2026-11-08 | Bug found by the campaign and fixed (`808ba8b`): B&C children inherited the node's own Farley bound even when truncated pricing (thousands of negative reduced costs) made it far weaker than the parent's bound, so fl3795 reported a global LB of -169966 (root LB 28569.75). Node bound is now max(parent, node) for pruning and children (still valid: a subproblem never has a lower optimum than its parent); node RCF keeps the node's own Farley bound. Band A re-run with the fix (fl3795 LB 28587); Bands C / D / E ran at `80f31c9` and do not use the B&C tree. fl3795 wall 3651.9 s for a 3600 s limit: the in-flight node batch finishes after the limit | engine/src/bnc/tree.cpp, docs/REPORT.md |
| 2026-11-08 | G5.5 fl3795 miss accepted (same path as G3.1 / G4.7): pr1002 proven in 1349 s, fl3795 gap 1.1754% at 3600 s; stronger cuts stay open work | 06 G5.5 |
| 2026-11-08 | Sprint 6 added by the operator (not in the kit): complete GUI app (projects/runs, live LP / cuts / tree view with provenance colors, heuristics lab, editor, benchmarks tab) and a historical benchmark pipeline (~40 TSPLIB instances vs known optima, published history and same-machine Concorde / LKH-3, test environment only). Plan: `docs/prompt/07_SPRINT6_GUI_APP_HISTORY_BENCH.md` | 07 |
| 2026-11-08 | Protocol v3 (M6.1; the plan said "v2", but Sprint 5 already shipped version 2): frame version 3, minimum 2. New engine -> GUI types LpSolution 0x25 (coalesced, <= 2 Hz, cap 2^20 edges), CutRound 0x26, TreeNode 0x27, Event 0x28 (UB / LB improvements with source and heuristic flag, phase, no violated cuts, tailing off, RCF, proven) and GUI -> engine Command 0x15 (executor arrives in M6.4; until then answered `command_unsupported`). The server stamps each frame with the negotiated version and never sends a v2 peer a type it does not know; Hello and Close carry version 2 in the header so any peer decodes them. Telemetry packet layout stays 2. FIFO queue cap 256 -> 4096 (tree and cut events). Live LB: the server also takes `global_lb` (root Farley bound, MASTER §5.5) but only before the first tree node, where the same loop starts reporting subtree bounds; tree progress is also emitted every second. C# client: `Connect` no longer starts the reader, `Start()` does (frames queued before the connection were dispatched before the GUI subscribed and were lost) | 07 M6.1, engine/src/ipc, gui/ApexTsp.Ipc |
| 2026-11-08 | App shell (M6.2): tabs Solve / Runs / Log now; Heuristics lab, Editor and Benchmarks tabs arrive with their modules (M6.4, M6.7) instead of empty placeholders. Panels are fixed, not dockable (the plan's "dockable side panels" skipped: no docking control in Avalonia 11 core; the side panel scrolls). The engine writes the run's report and tour straight into the history folder (`--report`, `--out`); the app adds `run.json` (settings, bounds history) when FINISHED arrives. Settings / history JSON use the System.Text.Json source generator (NativeAOT). Known optimum in the instance summary waits for `reference.csv` (M6.5) | 07 M6.2, gui/ |
| 2026-11-08 | Live solver view (M6.3). Cut highlight: the plan's "nodes of the last violated SEC / blossom flash" needs cut membership on the wire, which v3 does not carry; instead edges that are new in each LP (vs. the previous LP) flash yellow for 600 ms, so every cut round's effect on the support is visible. Cut rounds stream from the master cut loop (root and serial nodes); nodes solved by parallel tree workers report their cut totals in TreeNode (cuts panel shows rounds, tree panel click shows per-node cuts). The LP view shows the latest LP: a root round or the last merged tree node. Render budget: LP overlay measured up to 20000 nodes / 22000 LP edges (p95 11.22 ms headless); a 100000-node map with 110000 LP edges measured p95 16-17 ms, above the budget, but exact mode is not used at that size (scalable runs send no LP). LP overlay strokes: wide + antialiased only while <= 3000 edges of a kind are visible, hairlines beyond | 07 M6.3, gui/ |

---

## 10. Sprint 0 results (hardware: AMD Ryzen 5 3600X, 6 cores / 5 visible physical cores under WSL2, 25 GB RAM, Linux 6.18 WSL2, GCC 13.3, preset `benchmark`)

Command (seed 1, threads 1, `--time-limit 900 --mem-limit-mb 4096`; memory ceiling not enforced until Sprint 4):

```
build/benchmark/apex_tsp --instance data/<name>.tsp --mode scalable --seed 1 --threads 1 \
  --time-limit 900 --mem-limit-mb 4096 --out out/<name>.tour --report out/<name>.json --checkpoint out/ck_<name>
```

| Instance | UB0 (Hilbert) | UB after 2-opt + Or-opt | Gap vs. MASTER §7 | Wall (s) | Peak RSS |
|---|---|---|---|---|---|
| mona-lisa100K | 6569127 | 5940385 | 3.182% (vs. 5757191) | 0.90 | 29.6 MB |
| pla85900 | 188733742 | 155621179 | 9.298% | 0.54 | 27.8 MB |
| usa13509 | 28549025 | 21534569 | 7.765% | 0.07 | 7.7 MB |

Gap is computed offline against the reference value; the engine reports `gap_pct: null` until a valid LB exists.

---

## 11. Sprint 1 results (hardware: AMD Ryzen 5 3600X, 5 physical cores visible under WSL2, 25 GB RAM, GCC 13.3, preset `benchmark`)

Full transcript and procedure: [`docs/LP_GATE.md`](docs/LP_GATE.md).

| Gate | Result |
|---|---|
| G1.1 Netlib optima (5/5, rel. err <= 1e-6) | 5/5; worst rel. err 1.59e-11 (GROW22) |
| G1.2 vs. HiGHS 1.15.1 dual simplex, presolve off (time <= 10x, iterations <= 3x) | Devex 4.3x time / 1.51x iterations; DSE 3.2x / 1.16x |
| G1.3 pr1002-shaped stress LP | 6002 x 5263: optimal, 2951 it/s (Devex); 2002 x 5263: optimal, 13951 it/s |
| G1.4 Sprint 1 tests | green (see CI) |
| G1.5 ASan + UBSan | clean |

---

## 12. Sprint 2 results (hardware: AMD Ryzen 5 3600X, 5 physical cores visible under WSL2, 25 GB RAM, GCC 13.3, preset `benchmark`)

Transcripts: [`docs/SPRINT2_ROOT.md`](docs/SPRINT2_ROOT.md). The LB is the fully priced global bound
(no negative-reduced-cost edge anywhere in the complete graph). The gap is measured against the MASTER §7 optimum.

| Instance | Root LB | % of optimum | Gap vs. optimum | Gap vs. 2-opt UB | Rounds | Stop | Time |
|---|---|---|---|---|---|---|---|
| pr1002 | 258402.50 | 99.752% | 0.248% | 7.496% (UB 279341) | 97 | converged | 3.5 s |
| fl3795 | 28568.54 | 99.293% | 0.707% | 13.381% (UB 32982) | 415 | tailing-off | 335 s |

| Gate | Result |
|---|---|
| G2.1 pr1002 >= 97% of 259045, never above | 99.752% |
| G2.2 root gap <= 2% (vs. optimum) on pr1002 and fl3795 | 0.248% / 0.707% |
| G2.3 pr1002 <= 10 min, fl3795 <= 20 min | 3.5 s / 335 s |
| G2.4 Sprint 2 tests | green |
| G2.5 LB <= best known on all runs | yes (ratios above) |
| G2.6 ASan + UBSan | clean |

## 13. Sprint 3 results (hardware: AMD Ryzen 5 3600X, 5 physical cores visible under WSL2, 25 GB RAM, GCC 13.3, preset `benchmark`)

Transcripts: [`docs/SPRINT3_BNC.md`](docs/SPRINT3_BNC.md). Commit `e9f7235`, CI green.

| Instance | Status | UB | LB | Gap | Nodes | Time | Peak RSS |
|---|---|---|---|---|---|---|---|
| eil51 | OPTIMAL_PROVEN | 426 | 426 | 0 | 2 | 0.02 s | 4.4 MB |
| berlin52 | OPTIMAL_PROVEN | 7542 | 7542 | 0 | 1 | 0.00 s | 4.2 MB |
| a280 | OPTIMAL_PROVEN | 2579 | 2579 | 0 | 9 | 0.15 s | 6.6 MB |
| pr1002 (2 LPs) | TIME_LIMIT 1800 s | 259051 | 259020.82 | 0.0117% | 4871 | 1804 s | 1152 MB |
| fl3795 (2 LPs) | TIME_LIMIT 3600 s | 29271 | 28571.28 | 2.3905% | 93 | 3610 s | 1903 MB |

| Gate | Result |
|---|---|
| G3.1 optimality proven in limits | **MISS, accepted (§9)**: eil51 / berlin52 / a280 proven; pr1002 and fl3795 not proven, carried to Sprint 4 |
| G3.2 certificate integrity | OK on all 5 runs (UB == recomputed tour cost, LB <= optimum; LB_int == UB on proven runs) |
| G3.3 `T_rcf_never_prunes_optimal_edge` | green, 20 instances, 450 fixings at UB = opt |
| G3.4 `T_verification_pricing_planted_edge` | green, 6 removed / 6 priced back |
| G3.5 checkpoint equivalence | green (eil51 / a280, see §9 row) plus parallel resume |
| G3.6 pr1002 throughput | 2.70 nodes/s, LP 61.3% of time (2 parallel LPs) |
| G3.7 ASan + UBSan, determinism | clean; a280 logs bitwise identical over 2 runs, serial and parallel |

## 14. Sprint 4 results (hardware: AMD Ryzen 5 3600X, 5 physical cores visible under WSL2, 25 GB RAM, GCC 13.3, preset `benchmark`)

Transcripts: [`docs/SPRINT4_RESULTS.md`](docs/SPRINT4_RESULTS.md). Commit `5363ff5`, seed 1. Gap vs optimum = (UB - opt) / opt; certified gap = (UB - LB) / UB.

| Instance | Mode | Threads | Time | UB | vs optimum | LB (source) | Certified gap | Peak RAM |
|---|---|---|---|---|---|---|---|---|
| mona-lisa100K | scalable | 1 | 2700 s | 5765918 | 0.152% | 5751055 (HK) | 0.258% | 56 MB |
| mona-lisa100K | scalable | 5 | 600 s | 5764699 | 0.130% | 5658315 (HK) | 1.845% | 89 MB |
| pla85900 | scalable | 5 | 600 s | 143332990 | 0.668% | 141588258 (HK) | 1.217% | 81 MB |
| eil51 / berlin52 / a280 | exact | 1 | 0.19 / 0.09 / 0.72 s | 426 / 7542 / 2579 | 0 | = UB (LP) | 0, OPTIMAL_PROVEN | 5-6 MB |
| pr1002 | exact | 5 | 1800 s | 259062 | 0.007% | 259021.4 (LP) | 0.0157%, not proven | 480 MB |
| fl3795 | exact | 4 | 3600 s | 28935 | 0.567% | 28581.6 (LP) | 1.22%, not proven | 2207 MB |

| Gate | Result |
|---|---|
| G4.1 Band D mona-lisa100K <= 3% in <= 45 min, 1 thread | 0.152% in 45 min (stretch 2% met); tour sha256 cfaf3c5f2e5eea5689eeda120e7f31a4f382faf1f59071b6f3886338cdac3e8a |
| G4.2 pla85900 <= 2% in <= 8 h, RAM <= ceiling | 0.668% vs optimum, 1.217% certified, 10 min; 81 MB of 4096 MB |
| G4.3 mona-lisa100K <= 2% vs 5757191 in <= 8 h | 0.130% in 10 min; LB: HK 5658315, root LP not run at n = 100000 |
| G4.4 decomposition honesty audit | clean (no bound identifier in decomposition / primal code, no decomp / hk include in pruning code) |
| G4.5 FT vs PFI A/B | table pasted; FT default (loses 15% on stress, wins 26% on fl3795) |
| G4.6 memory enforcement | `T_mem_limit_graceful`, `T_lp_concurrency_budget` green |
| G4.7 Band A regression | **MISS**: eil51 / berlin52 / a280 proven; pr1002 (0.0157%) and fl3795 (1.22%) not proven in the Sprint 3 limits (same as the accepted G3.1 miss) |
| G4.8 TSan + ASan + UBSan | clean: 111 test cases / 604692 assertions under ASan+UBSan, 7 parallel tests under TSan |

---

## 15. Sprint 5 results and final results (hardware: AMD Ryzen 5 3600X, 5 physical cores visible under WSL2, 25 GB RAM, GCC 13.3, preset `benchmark`)

Transcripts: [`docs/SPRINT5_RESULTS.md`](docs/SPRINT5_RESULTS.md). Full campaign: [`docs/REPORT.md`](docs/REPORT.md). Engine commit `808ba8b`, seed 1, `--mem-limit-mb 4096`.

Final results (gap vs reference = (UB - ref) / ref; certified gap = (UB - LB) / UB with the engine's own LB):

| Instance | Mode | Threads | Time | UB | vs reference | LB (source) | Certified gap | Peak RAM |
|---|---|---|---|---|---|---|---|---|
| eil51 / berlin52 / a280 | exact | 5 | 0.1 / 0.1 / 0.5 s | 426 / 7542 / 2579 | 0 | = UB (LP) | 0, OPTIMAL_PROVEN | 5-8 MB |
| pr1002 | exact | 5 | 1349.2 s | 259045 | 0 | 259045 (LP) | 0, OPTIMAL_PROVEN | 557 MB |
| fl3795 | exact | 5 | 3651.9 s (limit 3600) | 28927 | 0.539% vs 28772 | 28587 (LP) | 1.1754%, not proven | 3137 MB |
| pla85900 | scalable | 5 | 600 s | 143356450 | 0.684% vs optimum | 141629726 (HK) | 1.2045% | 77 MB |
| mona-lisa100K | scalable | 5 | 600 s | 5764550 | 0.128% vs 5757191 (best known tour) | 5750673 (HK) | 0.2407% | 87 MB |
| mona-lisa100K | scalable | 1 | 2700 s | 5765259 | 0.140% vs 5757191 (best known tour) | 5751055 (HK) | 0.2464% | 54 MB |

| Gate | Result |
|---|---|
| G5.1 protocol robustness | `T_framing_fuzz` (0 frames from 1 MB garbage, 2000 CRC failures caught), `T_seqlock_reader_consistent` (973610 reads, 0 torn), `T_backpressure_drop_count` green; GUI decoder 5/5 |
| G5.2 control loop | live against `apex_tsp` in B&C: pause stall 5.4 ms, resume, checkpoint written, cancel to FINISHED 0.10 s, FINISHED == report bytes; `T_control_*` green |
| G5.3 GUI end-to-end | NativeAOT GUI + engine, pla85900 scalable 120 s: render p50 2.03 / p95 11.08 / p99 12.93 / max 20.48 ms (n = 3514); input-to-frame p95 11.23 ms; all 4 LOD levels; heatmap overlay; final report view == engine report |
| G5.4 campaign | `docs/REPORT.md`: Bands A, C, D, E + hardware context; Band B deferred (§9) |
| G5.5 regression | no regression: pr1002 now proven in 1349 s (Sprint 4: 0.0157%), fl3795 1.1754% (Sprint 4: 1.22%), Bands C / D at or better than Sprint 4. fl3795 not proven in 60 min: the accepted G3.1 / G4.7 miss, still open |
| G5.6 hygiene | ASan+UBSan 120 test cases / 606067 assertions clean; TSan 10 threaded tests, 0 warnings; NativeAOT publish succeeds (SDK 10.0.403) |
