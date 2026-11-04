#!/usr/bin/env bash
# Download the MASTER §7 reference instances into <repo>/data (idempotent).
set -euo pipefail
dir="${1:-$(cd "$(dirname "$0")/../.." && pwd)/data}"
mkdir -p "$dir"
cd "$dir"
base=https://comopt.ifi.uni-heidelberg.de/software/TSPLIB95/tsp
for i in eil51 berlin52 a280 pr1002 fl3795 d15112 d18512 usa13509 pla85900; do
    [ -s "$i.tsp" ] && continue
    curl -sfL --retry 3 -o "$i.tsp.gz" "$base/$i.tsp.gz"
    gunzip -f "$i.tsp.gz"
done
for i in eil51 berlin52 a280 pr1002; do
    [ -s "$i.opt.tour" ] && continue
    curl -sfL --retry 3 -o "$i.opt.tour.gz" "$base/$i.opt.tour.gz"
    gunzip -f "$i.opt.tour.gz"
done
[ -s mona-lisa100K.tsp ] || curl -sfL --retry 3 -o mona-lisa100K.tsp \
    https://www.math.uwaterloo.ca/tsp/data/ml/mona-lisa100K.tsp
# Netlib LPs (Sprint 1): uncompressed MPS copies from COIN-OR Data-Netlib
# (same problems as https://www.netlib.org/lp/data/, which ships them in compressed EMPS form).
mkdir -p netlib
for i in adlittle 25fv47 sc205 share2b grow22; do
    [ -s "netlib/$i.mps" ] && continue
    curl -sfL --retry 3 -o "netlib/$i.mps.gz" "https://raw.githubusercontent.com/coin-or-tools/Data-Netlib/master/$i.mps.gz"
    gunzip -f "netlib/$i.mps.gz"
done
ls -l "$dir" "$dir/netlib"
