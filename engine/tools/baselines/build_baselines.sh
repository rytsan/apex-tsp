#!/usr/bin/env bash
# Same-machine baselines (Sprint 6 M6.6), TEST ENVIRONMENT ONLY: downloads and builds Concorde
# (with QSopt) and LKH-3 into <repo>/third_party_test/ (gitignored). Nothing here is linked to or
# shipped with APEX-TSP. Licences: Concorde and QSopt are free for academic research use only
# (https://www.math.uwaterloo.ca/tsp/concorde.html); LKH is distributed for research use
# (README.txt of the package). Every download is pinned by sha256.
#
#   bash engine/tools/baselines/build_baselines.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TP="$ROOT/third_party_test"
mkdir -p "$TP/dl"
cd "$TP"

fetch() {  # fetch <url> <file> <sha256>
    local url=$1 out=$2 sum=$3
    if [[ ! -s "dl/$out" ]] || ! echo "$sum  dl/$out" | sha256sum -c --status; then
        curl -fsSL --retry 3 -o "dl/$out" "$url"
    fi
    echo "$sum  dl/$out" | sha256sum -c
}

# Concorde 03.12.19 (source) + QSopt (64-bit Linux build, ubuntu directory of the QSopt site).
fetch https://www.math.uwaterloo.ca/tsp/concorde/downloads/codes/src/co031219.tgz co031219.tgz \
    c3650a59c8d57e0a00e81c1288b994a99c5aa03e5d96a314834c2d8f9505c724
fetch https://www.math.uwaterloo.ca/~bico/qsopt/downloads/codes/ubuntu/qsopt.a qsopt.a \
    5dcf323c7fce85e8b9de7ce79aabc17b672e224b77e2a89370c4e35da07434ee
fetch https://www.math.uwaterloo.ca/~bico/qsopt/downloads/codes/ubuntu/qsopt.h qsopt.h \
    647729f1bd77e1263ecf35e1897c705ef1cb45e2d65dbd9cb8fdf5df5ae65624
# LKH-3.0.6: the author's site (webhotel4.ruc.dk) did not answer from the test machine, so the
# GitHub mirror cerebis/LKH3 is used, pinned to one commit (its README.txt: Version 3.0.6).
LKH_COMMIT=9a51de3161f2746e969f7a2e33220f637da64e54
fetch "https://codeload.github.com/cerebis/LKH3/tar.gz/$LKH_COMMIT" LKH3.tgz \
    16b4b385704f3860a40d1062231a57606e18bf5f4509da1f91c1e77505a651ca

rm -rf qsopt concorde lkh
mkdir -p qsopt lkh
cp dl/qsopt.a dl/qsopt.h qsopt/

echo "== LKH-3"
tar xzf dl/LKH3.tgz -C lkh --strip-components=1
make -C lkh -j"$(nproc)" > lkh_build.log 2>&1 || { tail -20 lkh_build.log; exit 1; }
test -x lkh/LKH

echo "== Concorde"
tar xzf dl/co031219.tgz
# -fcommon: the 2003 sources define globals in headers (GCC >= 10 defaults to -fno-common).
(cd concorde && CFLAGS="-O3 -fPIC -fcommon" ./configure --with-qsopt="$TP/qsopt" > ../concorde_configure.log 2>&1 \
    && make -j"$(nproc)" > ../concorde_build.log 2>&1) || { tail -30 concorde_configure.log concorde_build.log; exit 1; }
test -x concorde/TSP/concorde

{
    echo "built $(date -Iseconds) on $(uname -srm), $(gcc --version | head -1)"
    echo "concorde co031219 sha256 $(sha256sum dl/co031219.tgz | cut -d' ' -f1)"
    echo "qsopt (ubuntu) sha256 $(sha256sum dl/qsopt.a | cut -d' ' -f1)"
    echo "lkh 3.0.6 (github cerebis/LKH3@$LKH_COMMIT) sha256 $(sha256sum dl/LKH3.tgz | cut -d' ' -f1)"
} > VERSIONS.txt
cat VERSIONS.txt
echo "binaries: $TP/concorde/TSP/concorde  $TP/lkh/LKH"
