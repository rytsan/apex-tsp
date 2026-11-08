#!/usr/bin/env python3
"""Downloads the historical benchmark set (manifest.csv) into data/tsplib/ and verifies sha256.

  python3 engine/tools/history/fetch_tsplib.py [--check]

Files already in data/ with the pinned checksum are copied instead of downloaded. --check only
verifies what is on disk. Exit 1 on any checksum mismatch or missing file.
"""
import csv
import gzip
import hashlib
import os
import shutil
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "data", "tsplib")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    check = "--check" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    rows = list(csv.DictReader(open(os.path.join(HERE, "manifest.csv"))))
    for r in rows:
        dst = os.path.join(OUT, r["instance"] + ".tsp")
        if not check and not (os.path.exists(dst) and sha(dst) == r["sha256"]):
            local = os.path.join(ROOT, "data", r["instance"] + ".tsp")
            if os.path.exists(local) and sha(local) == r["sha256"]:
                shutil.copyfile(local, dst)
            else:
                req = urllib.request.Request(r["url"], headers={"User-Agent": "apex-tsp-reference/1"})
                raw = urllib.request.urlopen(req, timeout=300).read()
                if r["url"].endswith(".gz"):
                    raw = gzip.decompress(raw)
                with open(dst + ".tmp", "wb") as f:
                    f.write(raw)
                os.replace(dst + ".tmp", dst)
        ok = os.path.exists(dst) and sha(dst) == r["sha256"]
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} {r['instance']:<14} {r['sha256'][:16]}  {r['url']}")
    print(f"{len(rows) - bad}/{len(rows)} verified in {OUT}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
