# APEX-TSP GUI

Avalonia 11 + SkiaSharp client of the engine IPC (Sprint 5, M5.3).

- `ApexTsp.Ipc`: framing, CRC32, protocol structs, UDS / named-pipe client, SharedTour reader.
- `ApexTsp.Render`: TSPLIB coordinates, viewport, LOD renderer (raw, decimated, clusters, heatmap).
- `ApexTsp.Gui`: window, map view, UB/LB chart, panels, controls, scripted demo.
- `ApexTsp.Tests`: xUnit (`T_gui_struct_sizes_match_engine`, `T_lod_path_executes`, `T_vm_gap_chart_updates`, client against the engine).

```bash
dotnet test ApexTsp.Tests -c Release            # needs build/dev-gcc/apex_tsp (or APEX_BIN)
dotnet publish ApexTsp.Gui -c Release -r linux-x64 -p:PublishAot=true -o aot   # SDK >= 10.0.403

# launch the engine from the GUI (engine arguments after --)
aot/ApexTsp.Gui --engine ../build/dev-gcc/apex_tsp --instance ../data/pla85900.tsp -- --mode scalable --time-limit 600 --threads 5
# attach to a running engine started with --ipc <endpoint> --shared-tour <file>
aot/ApexTsp.Gui --attach /tmp/apex.sock --shared-tour /tmp/apex.tour --instance ../data/pla85900.tsp
```

Map: drag = pan, wheel = zoom, double click = fit. `--demo-out <dir>` runs a scripted pan/zoom
sequence, saves screenshots, `frame_stats.txt` and the final report, then exits. Under WSL use
`DISPLAY=:0`.
