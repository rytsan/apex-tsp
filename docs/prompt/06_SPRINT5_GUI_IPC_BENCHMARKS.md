# SPRINT 5 — GUI, IPC, FINAL BENCHMARK CAMPAIGN

> Paste **00_MASTER_PROMPT.md** together with this file.
**Existing (stable):** Sprints 0–4 (complete engine: primal, LP, cuts, B&C, decomposition,
memory enforcement, HK bound). The engine-side telemetry **stub** from Sprint 0
(`TelemetrySink`) is replaced here by the real IPC implementation.
**Builds:** IPC protocol (engine + C# client), Avalonia/SkiaSharp GUI with LOD, benchmark
campaign tooling and final report.
**Modules:** M5.1 → M5.4, one per response, in order.

---

## M5.1 — Wire protocol + shared route memory (`src/ipc/protocol.{hpp,cpp}`)

- **Framing:** `[u32 length][u16 version][u8 msg_type][u8 flags][payload]`; `flags.0` =
  payload carries CRC32 (implement CRC32; verify with `T_crc32_known_vectors`).
- **Handshake:** engine and client exchange `{version, protocol_min, engine_config
  (mode, mem_limit_mb, threads), gui_capabilities}` before any other message; version
  mismatch => structured close.
- **GUI -> engine (control):** `PAUSE`, `RESUME`, `CANCEL`, `CHECKPOINT`, `SHUTDOWN` — all
  idempotent, acknowledged with `ACK(seq)`.
- **Engine -> GUI:** `TELEMETRY`, `TOUR_UPDATE`, `CLUSTER_MAP`, `LOG`, `FINISHED`.
- `TELEMETRY` payload (MASTER v2 struct, both sides `static_assert` the size):

```csharp
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct TelemetryPacket {
    public ulong  MagicVersion;   // 'ATSP' | protocol version 2
    public ulong  Seq;            // monotonic; drops duplicates/reorder
    public ulong  TimestampUs;
    public uint   Mode;           // 0 exact, 1 scalable
    public uint   LbSource;       // 0 lp, 1 lagrangian
    public uint   Phase;          // pipeline phase id
    public uint   OpenNodes;
    public uint   ActiveEdges;
    public uint   PrunedEdgesRcf;
    public uint   CutPoolSize;
    public double UpperBound;
    public double LowerBound;
    public double GapPct;
    public ulong  SimplexIterations;
    public ulong  Refactorizations;
    public double IterPerSec;
    public ulong  RamBytes;       // physical working set (VmHWM / WorkingSet)
    public ulong  ColdBytes;      // mmap/persisted
    public uint   ThreadsLp;
}
```
- Emission policy: 1 Hz heartbeat + **immediate** on UB or LB improvement (coalesced — max 1
  in-flight packet per channel; never block the solver: bounded queue, drop-with-counter on
  overflow).
- **Shared route memory:** `SharedTour` layout — header `{uint64 epoch (seqlock), uint32 count,
  uint32 capacity}` + `uint32 tour[]`; writer increments epoch to odd before writing, to even
  after (release semantics); reader retries on odd/changed epoch. The GUI never blocks the
  engine. Tour arrives via mmap-backed file (MASTER §2, directive 4 — cold-data policy).
- Tests: `T_framing_fuzz` (1 MB of random bytes; malformed frames dropped, no crash, engine
  alive), `T_seqlock_reader_consistent` (concurrent writer/reader, 10^6 reads, zero torn
  snapshots), `T_backpressure_drop_count`.

## M5.2 — Engine IPC server (`src/ipc/server.{hpp,cpp}`)

- Named pipes (Windows) / UDS (Linux) behind one transport interface; server thread with the
  bounded queue from M5.1; control messages wired to the engine's pause/cancel/checkpoint paths
  (these exist since Sprint 3/4 — no new engine semantics allowed here).
- `CLUSTER_MAP` message: for the decomposition phase, per-tick map of
  `(cluster_id: uint32, state: uint8 {idle, solving, patching})` — emitted only in scalable
  mode, max 1 per second.
- `FINISHED` payload = the final JSON report schema (below), byte-identical to `--report`:
  `{instance, mode, status, ub, lb, lb_source, gap_pct, wall_s, ram_peak_bytes, disk_bytes,
  iterations, refactorizations, cuts_by_family, tours_found, seed, threads, hardware,
  mem_limit_mb, verification_rounds}`.
- Tests: `T_control_pause_resume` (pause actually stalls the solver within 200 ms; resume
  continues deterministically from the same seed), `T_finished_matches_report_file`.

## M5.3 — Avalonia + SkiaSharp GUI (`gui/`)

- Projects: `ApexTsp.Ipc` (pipe/UDS client, framing, structs with `static_assert` parity via
  generated size check), `ApexTsp.Render` (SkiaSharp), `ApexTsp.Gui` (Avalonia 11 views +
  view models), `ApexTsp.Tests` (xUnit).
- Canvas: nodes as one batched point draw; tour as a single polyline; **LOD mandatory**:
  > 50k elements on screen => cluster-aggregated/decimated drawing; detail increases with
  zoom (levels: raw => decimated => clusters => heatmap). Heatmap overlay from `CLUSTER_MAP`.
- Panels: UB/LB/gap chart (rolling), phase indicator, cut pool size, iterations/s, RAM vs.
  ceiling bar, `lb_source` badge, controls PAUSE/RESUME/CANCEL/CHECKPOINT, final report view.
- Pan/zoom: fluid with 100k points (input latency <= 50 ms; render <= 16 ms/frame with LOD —
  measure in a headless render test, do not claim FPS you cannot measure: report the measured
  frame time distribution instead).
- NativeAOT publish profile (`dotnet publish -r <rid> -p:PublishAot=true`); if SkiaSharp GL
  fails under AOT on the target platform, ship the documented CPU-render fallback
  (WriteableBitmap) — the requirement is LOD + fluidity, not the API (MASTER §3).
- Tests (xUnit): `T_gui_struct_sizes_match_engine` (byte sizes of all protocol structs),
  `T_lod_path_executes` (headless render of 100k synthetic points through both LOD branches),
  `T_vm_gap_chart_updates` (feed synthetic telemetry stream).

## M5.4 — Benchmark campaign + final report (`tools/run_benchmarks.sh`, `docs/REPORT.md`)

- Script runs the full matrix and writes per-run JSON + aggregated `docs/REPORT.md`:

| Band | Instances / mode | Limits | Pass criterion |
|---|---|---|---|
| A | eil51, berlin52, a280, pr1002, fl3795 — exact | per MASTER §8.4 | optimality proven + certificate |
| B (stretch) | usa13509 — exact | optimality <= 24 h or gap <= 0.5% in <= 4 h | report actual |
| C | pla85900, mona-lisa100K — scalable | <= 8 h, RAM ceiling | gap <= 2% (mona-lisa vs. 5757191) |
| D | mona-lisa100K — primal standalone | <= 45 min, 1 thread | gap <= 3% (stretch 2%) |
| E | Netlib + stress LP — LP bench | per Sprint 1 gate | within published bounds |

- Every table row carries: hardware (§3), seed, threads, `--mem-limit-mb`, date, commit hash.
- Context baselines (optional, test environment only): LKH-3 / Concorde rows where installable,
  clearly marked "context only — not part of the product".
- `docs/REPORT.md` must state, for mona-lisa100K, the comparison **against 5757191 (best
  known tour)** and must NOT use the word "optimal" for it (MASTER §7).
- Regression: re-run Band A and confirm all Sprint 3/4 limits still hold.

---

## SPRINT 5 GATE (all must pass; paste real output)

| # | Criterion | Target |
|---|---|---|
| G5.1 | Protocol robustness | `T_framing_fuzz`, `T_seqlock_reader_consistent`, `T_backpressure_drop_count` green |
| G5.2 | Control loop | pause/resume/cancel/checkpoint exercised live against a running solver; `T_control_*` green |
| G5.3 | GUI end-to-end | engine + GUI on pla85900 scalable run: telemetry, LOD pan/zoom, heatmap, final report view; frame-time distribution pasted |
| G5.4 | Campaign executed | `docs/REPORT.md` generated with Bands A, C, D, E complete + hardware context; Band B attempted or explicitly deferred |
| G5.5 | Regression | Band A limits still hold; no gate from Sprints 0–4 broken |
| G5.6 | Hygiene | ASan/UBSan/TSan clean; NativeAOT publish succeeds or fallback documented and demonstrated |

**Deliverables:** file tree diff, full code (engine IPC + gui/), `docs/REPORT.md`,
`docs/LP_GATE.md` update if the LP gate was re-run, README final "results" section.
