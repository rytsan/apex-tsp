#!/usr/bin/env bash
# Final benchmark campaign (Sprint 5, M5.4). Runs the bands in sequence (one run at a time, so
# timings are not shared), writes per-run JSON + logs under out/campaign/<stamp>/ and regenerates
# docs/REPORT.md from them.
#
#   BANDS="A C D E" THREADS=5 MEM_MB=4096 bash engine/tools/run_benchmarks.sh
#
# Env: BANDS (default "A C D E"; add B for usa13509, 4 h), THREADS (5), MEM_MB (4096), SEED (1),
#      LIMIT_C (600 s per Band C run; the band allows 8 h), HIGHS_PY (python with highspy, test
#      environment only, for the Band E comparison), SKIP_BUILD=1, OUT (output dir).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BANDS="${BANDS:-A C D E}"
THREADS="${THREADS:-5}"
MEM_MB="${MEM_MB:-4096}"
SEED="${SEED:-1}"
LIMIT_C="${LIMIT_C:-600}"
OUT="${OUT:-out/campaign/$(date +%Y%m%d-%H%M%S)}"
BIN=build/benchmark/apex_tsp
mkdir -p "$OUT"

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
    (cd engine && cmake --preset benchmark >/dev/null && cmake --build --preset benchmark --target engine lp_bench root_bench)
fi

# Context for every row: commit, date, hardware, toolchain.
python3 - "$OUT/meta.json" "$THREADS" "$MEM_MB" "$SEED" <<'EOF'
import json, platform, subprocess, sys, datetime, os
def sh(c):
    try: return subprocess.check_output(c, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
    except subprocess.CalledProcessError: return ""
cpu = sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip()
meta = {
    "commit": sh("git rev-parse --short HEAD"),
    "dirty": sh("git status --porcelain --untracked-files=no") != "",
    "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
    "cpu": cpu, "logical_cpus": os.cpu_count(),
    "ram_gb": round(int(sh("grep MemTotal /proc/meminfo | awk '{print $2}'") or 0) / 1048576, 1),
    "os": platform.platform(), "compiler": sh("g++ --version | head -1"),
    "preset": "benchmark (-O3 -DNDEBUG -ffp-contract=off -march=x86-64-v3)",
    "threads": int(sys.argv[2]), "mem_limit_mb": int(sys.argv[3]), "seed": int(sys.argv[4]),
}
json.dump(meta, open(sys.argv[1], "w"), indent=1)
EOF

# run <band> <instance> <mode> <time limit s> <threads>
run() {
    local band=$1 name=$2 mode=$3 limit=$4 threads=$5 tag="$1_$2"
    echo "[$(date +%T)] band $band: $name $mode limit=${limit}s threads=$threads"
    printf '{"band":"%s","instance":"%s","mode":"%s","limit_s":%s,"threads":%s}\n' \
        "$band" "$name" "$mode" "$limit" "$threads" > "$OUT/$tag.run.json"
    local rc=0
    "$BIN" --instance "data/$name.tsp" --mode "$mode" --time-limit "$limit" --threads "$threads" \
        --mem-limit-mb "$MEM_MB" --seed "$SEED" --report "$OUT/$tag.json" --out "$OUT/$tag.tour" \
        > "$OUT/$tag.log" 2>&1 || rc=$?
    echo "  exit=$rc $(cat "$OUT/$tag.json" 2>/dev/null | head -c 220)"
}

for band in $BANDS; do
    case $band in
        A)  # optimality proof, exact mode (MASTER §8.4 limits; small instances capped at 60 s)
            run A eil51 exact 60 "$THREADS"
            run A berlin52 exact 60 "$THREADS"
            run A a280 exact 60 "$THREADS"
            run A pr1002 exact 1800 "$THREADS"
            run A fl3795 exact 3600 "$THREADS" ;;
        B)  run B usa13509 exact 14400 "$THREADS" ;;
        C)  run C pla85900 scalable "$LIMIT_C" "$THREADS"
            run C mona-lisa100K scalable "$LIMIT_C" "$THREADS" ;;
        D)  run D mona-lisa100K scalable 2700 1 ;;
        E)  echo "[$(date +%T)] band E: lp_bench (Netlib + stress), root_bench pr1002 (E2)"
            build/benchmark/lp_bench --pricing devex --write-mps "$OUT/stress_pr1002.mps" > "$OUT/E_lp_devex.txt" 2>&1
            build/benchmark/lp_bench --pricing dse > "$OUT/E_lp_dse.txt" 2>&1
            build/benchmark/root_bench --instance data/pr1002.tsp --ref 259045 > "$OUT/E2_root_pr1002.txt" 2>&1
            if [[ -n "${HIGHS_PY:-}" ]]; then
                "$HIGHS_PY" engine/tools/highs_compare.py data/netlib/{adlittle,25fv47,sc205,share2b,grow22}.mps \
                    "$OUT/stress_pr1002.mps" > "$OUT/E_highs.txt" 2>&1
            fi ;;
        *)  echo "unknown band $band" >&2; exit 2 ;;
    esac
done

python3 engine/tools/make_report.py "$OUT" > docs/REPORT.md
echo "report: docs/REPORT.md (runs in $OUT)"
