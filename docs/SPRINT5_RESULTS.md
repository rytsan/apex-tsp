# Sprint 5 — gate transcripts

Hardware: AMD Ryzen 5 3600X (5 physical cores visible under WSL2), 25 GB RAM, Linux 6.18 WSL2, GCC 13.3.
Engine commit `808ba8b` (B&C bound fix) unless noted; seed 1 everywhere. GUI: .NET SDK 10.0.403, Avalonia 11.3.22,
SkiaSharp 2.88.9.

## G5.1 — protocol robustness

`build/dev-gcc/engine_tests -tc="T_framing_fuzz,T_seqlock_reader_consistent,T_backpressure_drop_count,T_crc32_known_vectors,T_frame_roundtrip_and_handshake,T_telemetry_throttle,T_control_*" -s`

```
test_ipc_protocol.cpp:110: MESSAGE: fuzz: frames accepted from garbage=0 dropped_bytes=1312609 crc_failures=2000 buffered=7
test_ipc_protocol.cpp:134: SUCCESS: CHECK( q.coalesced() == 999 ) is correct!
test_ipc_protocol.cpp:142: SUCCESS: CHECK( q.dropped() == 6 ) is correct!
test_ipc_protocol.cpp:167: SUCCESS: CHECK( slow.dropped() == 50000 - 8 ) is correct!
test_ipc_protocol.cpp:220: MESSAGE: seqlock: reads=973610 torn=0 last_epoch=77352346
[doctest] test cases:    9 |    9 passed | 0 failed | 111 skipped
[doctest] assertions: 1366 | 1366 passed | 0 failed |
```

GUI side (C# decoder, struct sizes parsed from `protocol.hpp`): `dotnet test ApexTsp.Tests -c Release`

```
Passed!  - Failed:     0, Passed:     5, Skipped:     0, Total:     5, Duration: 2 s - ApexTsp.Tests.dll (net10.0)
```

## G5.2 — control loop

Same run as G5.1. `T_control_live_and_T_finished_matches_report_file` starts the real `apex_tsp` with `--ipc`,
on pr1002 exact inside branch-and-cut, then sends Pause (ack 1, `paused stalled_threads=`), Pause again (2,
idempotent), Resume (3), Checkpoint (4, `checkpoint_written`, file non-empty), reads the shared tour (valid
permutation of 1002), Cancel (5), and compares the FINISHED payload with the `--report` file bytes.

```
test_ipc_server.cpp:92:  MESSAGE: pause: reference run 797.626 ms, stalled after 0.206325 ms, held 300.32 ms
test_ipc_server.cpp:116: MESSAGE: cancel: solver returned after 0.164169 ms
test_ipc_server.cpp:196: MESSAGE: engine: paused stalled_threads=1 after_ms=5.4
test_ipc_server.cpp:208: SUCCESS: REQUIRE( pump(is_log("checkpoint_written"), 10) ) is correct!
test_ipc_server.cpp:224: SUCCESS: REQUIRE( pump(is_ack(5), 5) ) is correct!
test_ipc_server.cpp:230: MESSAGE: live: telemetry=8 tour_updates=5 cancel_to_finished_s=0.101077 rc=0
```

## G5.3 — GUI end-to-end, pla85900 scalable

NativeAOT binary, engine `build/benchmark/apex_tsp`, WSLg (`DISPLAY=:0`):

`ApexTsp.Gui --engine build/benchmark/apex_tsp --instance data/pla85900.tsp --demo-out g53 -- --mode scalable --time-limit 120 --threads 5 --report g53/engine_report.json`

`frame_stats.txt` (scripted pan/zoom through the pointer path while the engine runs):

```
nodes 85900, map 1040x900 px, CPU render (SkiaSharp -> WriteableBitmap)
render ms per frame: n=3514 p50=2.03 p95=11.08 p99=12.93 max=20.48 ms
input-to-rendered-frame ms (scripted input through the pointer path): n=3436 p50=2.13 p95=11.23 p99=13.61 max=111.34 ms
frames at Raw: 2787
frames at Decimated: 525
frames at Clusters: 102
frames at Heatmap: 100
```

Engine log (telemetry source), report view check:

```
t=19.304 event=construct phase="decomposition (heuristic)" heuristic=true leaves=146 levels=8 ... ub=144598470
t=39.713 event=held_karp lb_source=lagrangian lb=140672276.0747 lb_int=140672277 iterations=156
t=120.066 event=done status=done mode=scalable ub=143529420 lb=140672277 gap_pct=1.9906 lb_source=lagrangian wall_s=120.065 ram_peak_bytes=130805760
$ cmp report.json engine_report.json && echo "FINISHED == --report bytes"
FINISHED == --report bytes
```

291 `cluster_box` events reached the heatmap overlay. Screenshots: [final window](img/gui_pla85900_final.png)
(UB/LB chart, LB badge, RAM bar, final report view), [heatmap LOD](img/gui_pla85900_heatmap.png),
[cluster overlay](img/gui_pla85900_clusters.png).

## G5.4 — campaign

[`docs/REPORT.md`](REPORT.md): Bands A (re-run at `808ba8b`), C, D, E (at `80f31c9`; scalable mode and LP
do not use the B&C tree) with hardware context; Band B deferred (README §9).

| Band | Result |
|---|---|
| A | eil51 / berlin52 / a280 / pr1002 OPTIMAL_PROVEN (pr1002 1349.2 s of 1800); fl3795 TIME_LIMIT, gap 1.1754% |
| B | deferred |
| C | pla85900 1.2045% certified (0.6839% vs optimum); mona-lisa100K 0.1278% vs 5757191 (best known tour); 600 s each, PASS |
| D | mona-lisa100K 1 thread 2700 s: 0.1401% vs 5757191, PASS (stretch met) |
| E | E1 PASS (Devex 2.9x time / 1.54x it, DSE 1.9x / 0.98x vs HiGHS); E2 pr1002 root 99.7388%, PASS |

## G5.5 — regression

```
t=1349.183 event=done status=OPTIMAL_PROVEN mode=exact ub=259045 lb=259045 gap_pct=0.0000 lb_source=lp wall_s=1349.183 ram_peak_bytes=584433664
t=3651.944 event=done status=TIME_LIMIT mode=exact ub=28927 lb=28587 gap_pct=1.1754 lb_source=lp wall_s=3651.943 ram_peak_bytes=3289079808
```

| Sprint 4 value | Sprint 5 value |
|---|---|
| pr1002 gap 0.0157% at 1800 s | OPTIMAL_PROVEN at 1349.2 s |
| fl3795 gap 1.22% at 3600 s | gap 1.1754% at 3651.9 s |
| G4.1 mona-lisa100K 1 thread 0.152% | 0.1401% |
| G4.2 pla85900 1.217% certified | 1.2045% |
| G4.3 mona-lisa100K 0.130% | 0.1278% |

Sprint 0–4 tests: full suite green under ASan+UBSan (G5.6). fl3795 remains the accepted G3.1 / G4.7 miss.

## G5.6 — hygiene

```
$ time build/asan/engine_tests            # ASan + UBSan, full suite
[doctest] test cases:    120 |    120 passed | 0 failed | 0 skipped
[doctest] assertions: 606067 | 606067 passed | 0 failed |
asan exit=0
ASan/UBSan findings: 0
real	10m4.142s

$ setarch -R build/tsan/engine_tests -tc="T_parallel_tree_deterministic,T_parallel_checkpoint_resume,T_lp_concurrency_budget,T_leaf_parallel_deterministic_option,T_seqlock_reader_consistent,T_pool_deterministic_merge,T_control_*,T_background_thread_no_races"
[doctest] test cases:   10 |   10 passed | 0 failed | 110 skipped
[doctest] assertions: 1732 | 1732 passed | 0 failed |
TSan warnings: 0

$ dotnet publish ApexTsp.Gui -c Release -r linux-x64 -p:PublishAot=true -o aot     # SDK 10.0.403
  Generating native code
  ApexTsp.Gui -> aot/
aot/ApexTsp.Gui: ELF 64-bit LSB pie executable, x86-64, 18194616 bytes
```

The G5.3 run above uses this AOT binary. CI (`.github/workflows/ci.yml`, job `gui`) runs the GUI tests and the AOT publish.
