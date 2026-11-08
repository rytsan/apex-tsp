# APEX-TSP GUI

Avalonia 11 + SkiaSharp client of the engine IPC (Sprint 5, M5.3).

- `ApexTsp.Ipc`: framing, CRC32, protocol structs, UDS / named-pipe client, SharedTour reader.
- `ApexTsp.Render`: TSPLIB coordinates, viewport, LOD renderer (raw, decimated, clusters, heatmap).
- `ApexTsp.Gui`: window, map view, UB/LB chart, panels, controls, scripted demo.
- `ApexTsp.Tests`: xUnit (`T_gui_struct_sizes_match_engine`, `T_lod_path_executes`, `T_vm_gap_chart_updates`, client against the engine).

```bash
dotnet test ApexTsp.Tests -c Release            # needs build/dev-gcc/apex_tsp (or APEX_BIN)
dotnet publish ApexTsp.Gui -c Release -r linux-x64 -p:PublishAot=true -o aot   # SDK >= 10.0.403

aot/ApexTsp.Gui                                  # app: open an instance, configure, run
aot/ApexTsp.Gui ../data/pr1002.tsp               # open an instance at start
aot/ApexTsp.Gui ../data/pla85900.tsp --engine ../build/benchmark/apex_tsp
# attach to a running engine started with --ipc <endpoint> --shared-tour <file>
aot/ApexTsp.Gui --attach /tmp/apex.sock --shared-tour /tmp/apex.tour --instance ../data/pla85900.tsp
```

App (Sprint 6 M6.2): toolbar Open (Ctrl+O, drag and drop, Recent), Start (F5), Pause (F6),
Resume (F7), Cancel (Shift+F5), Checkpoint, Save tour (Ctrl+S, TSPLIB `.tour`), Export (map PNG,
tour SVG, report JSON), theme (system / light / dark). Tabs: **Solve** (map, instance summary, run
settings: mode, time limit, threads, seed, memory ceiling, checkpoint dir, resume; bounds, chart,
final report; below the map: bounds over time with event markers, cuts per round, live branch-and-cut tree; provenance badges and colors: orange heuristic, blue LP, purple Lagrangian, green proven; map layers tour / nodes / LP support / new LP edges / clusters), **Runs** (history of the instance or of all instances; double click reloads the
tour, bounds and report), **Log** (colored by category: heuristic, LP, cuts, tree, system; text and category filters).

Engine binary: `--engine`, else the last one used (settings), `$APEX_TSP_ENGINE`, next to the GUI,
or the repo's `build/<preset>/apex_tsp`. Settings and history live in `$APEX_TSP_HOME` (default:
the user's local app data folder, `apex-tsp/`): `settings.json` and
`history/<instance>-<hash>/<run>/{run.json, report.json, tour.tour, engine.log}`.

Map: drag = pan, wheel = zoom, double click = fit. Scripts (evidence): `--demo-out <dir> -- <engine
args>` (G5.3: pan/zoom, LOD screenshots, `frame_stats.txt`), `--demo-app <dir> [-- <engine args>]`
(G6.2: configure, run, pause / resume, save tour, reload from history, dark theme; `app_demo.log`
with PASS/FAIL lines). Under WSL use `DISPLAY=:0`.
