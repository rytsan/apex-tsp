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
[ -s mona-lisa100K.tsp ] || curl -sfL --retry 3 -o mona-lisa100K.tsp \
    https://www.math.uwaterloo.ca/tsp/data/ml/mona-lisa100K.tsp
ls -l "$dir"
