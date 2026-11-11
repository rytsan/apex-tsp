#!/usr/bin/env bash
# Opens the GUI (native AOT build in gui/aot) with the benchmark engine. On WSL the X display is
# WSLg's :0; a DISPLAY pointing at an old Windows X server (VcXsrv) makes Avalonia fail.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -n "$WSL_DISTRO_NAME" ] && export DISPLAY=:0
exec "$ROOT/gui/aot/ApexTsp.Gui" --engine "$ROOT/build/benchmark/apex_tsp" "$@"
