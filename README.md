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
| 2026-11-04 | Root loop tailing-off stop (gain < 1e-4 relative over 50 rounds), reported separately from convergence | M2.6 |
| 2026-11-04 | Iterated SEC shrinking: contract a supernode pair only if both have x(delta) = 2 (merging across c > 1 hides violated SECs; found by `T_shrinking_safe`) | MASTER §6, M2.2 |
| 2026-11-04 | Blossom inequality corrected to the TSP 2-matching form `x(delta(H)\T) - x(T) >= 1 - |T|` (handle + odd teeth); the original `x(E(S)) <= (|S|-1)/2` is invalid for the TSP. Heuristic separation default, Padberg-Rao behind `--sep-blossom=full` | MASTER §6, M2.4 |
| 2026-11-04 | Dual phase 1 by artificial bounds (Koberstein), **only** when the initial basis is not dual feasible (generic LPs such as Netlib). TSP LPs (c >= 0, 0 <= x <= 1) never trigger it; asserted by tests | M1.4 |

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
