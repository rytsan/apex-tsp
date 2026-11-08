#!/usr/bin/env python3
"""Builds the historical reference table (Sprint 6 M6.5) from its public sources.

  python3 engine/tools/history/build_reference.py            # writes reference.csv + manifest.csv

Every value comes from one of the SOURCES pages and names it in the row; a field no source gives
stays empty (never guessed). manifest.csv pins each instance file (URL + sha256 of the
uncompressed .tsp) for fetch_tsplib.py.
"""
import csv
import gzip
import hashlib
import html
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TSPLIB = "https://comopt.ifi.uni-heidelberg.de/software/TSPLIB95/tsp/"
SOURCES = {
    "tsplib_best": TSPLIB + "TSP-BEST.html",  # optimal values (Reinelt)
    "milestones": "https://www.math.uwaterloo.ca/tsp/history/milestone.html",
    "concorde99": "https://www.math.uwaterloo.ca/tsp/concorde/benchmarks/bench99.html",
    "concorde03": "https://www.math.uwaterloo.ca/tsp/concorde/benchmarks/bench.html",
    "monalisa": "https://www.math.uwaterloo.ca/tsp/data/ml/monalisa.html",
}
HW99 = "Compaq XP1000 500 MHz, Concorde 99.12.15 (-s 99)"
HW03 = "Intel Xeon 2.8 GHz (1 CPU), Concorde 03.12.19, CPLEX 6.5"
MONA_URL = "https://www.math.uwaterloo.ca/tsp/data/ml/mona-lisa100K.tsp"

# Benchmark set: TSPLIB instances in the engine's metrics (EUC_2D / CEIL_2D) up to ~1000 nodes,
# the larger project references, the solved milestones in these metrics, and mona-lisa100K.
SET = """berlin52 eil51 st70 eil76 pr76 rat99 kroA100 rd100 eil101 lin105 pr107 pr124 bier127 ch130 pr136
pr144 ch150 kroA150 pr152 u159 rat195 d198 kroA200 ts225 tsp225 pr226 pr264 a280 pr299 lin318 rd400 fl417
pr439 pcb442 d493 u574 rat575 p654 d657 u724 rat783 dsj1000 pr1002 pr2392 fl3795 pla7397 usa13509 d15112
pla85900 mona-lisa100K""".split()
# Milestones in other metrics: listed for the history, not run (the engine reads EUC_2D / CEIL_2D).
HISTORY_ONLY = {"dantzig42": "EXPLICIT", "gr120": "EXPLICIT", "att532": "ATT", "gr666": "GEO"}


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "apex-tsp-reference/1"})
    return urllib.request.urlopen(req, timeout=300).read()


def text(url):
    t = get(url).decode("latin1")
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", " ", t)))


def num(s):
    return s.replace(",", "")


def parse_best(t):
    out = {}
    for name, value, metric in re.findall(r"([A-Za-z]+\d+)\s*:\s*(\d+)(?:\s*\((\w+)\))?", t):
        out.setdefault(name, {})[metric or ""] = int(value)
    return out


def parse_milestones(t):
    rows = {}
    body = t[t.find("Name") :]
    for year, team, name in re.findall(r"(\d{4}) (.+?) [\d,]+ cities (\S+)", body):
        rows[name] = (int(year), team.strip())
    return rows


def parse_bench(t, names, order):
    """order 'nodes_time' (1999 page) or 'time_nodes' (2003 page) -> {name: (seconds|None, nodes, raw)}."""
    out = {}
    tok = t.split(" ")
    for i, w in enumerate(tok):
        if w not in names or w in out or i + 2 >= len(tok):
            continue
        a, b = tok[i + 1], tok[i + 2]
        if order == "nodes_time" and re.fullmatch(r"[\d,]+", a):
            raw = b
            if b.startswith("~"):  # "~4 years (5)"
                raw = " ".join(tok[i + 2 : i + 4] + ([tok[i + 4]] if i + 4 < len(tok) and tok[i + 4].startswith("(") else []))
                out[w] = (None, int(num(a)), raw)
            elif re.fullmatch(r"[\d.]+", b):
                note = tok[i + 3] if i + 3 < len(tok) and re.fullmatch(r"\(\d\)", tok[i + 3]) else ""
                out[w] = (float(b), int(num(a)), (b + " " + note).strip())
        elif order == "nodes_time" and a == "--" and b == "OPEN":
            out[w] = (None, None, "OPEN")  # unsolved when the page was written
        elif order == "time_nodes" and re.fullmatch(r"[\d.]+", a) and re.fullmatch(r"\d+", b):
            out[w] = (float(a), int(b), a)
    return out


def file_info(name):
    url = MONA_URL if name == "mona-lisa100K" else TSPLIB + name + ".tsp.gz"
    raw = get(url)
    if url.endswith(".gz"):
        raw = gzip.decompress(raw)
    head = raw[:4000].decode("latin1")
    dim = int(re.search(r"DIMENSION\s*:\s*(\d+)", head).group(1))
    ewt = re.search(r"EDGE_WEIGHT_TYPE\s*:\s*(\S+)", head).group(1)
    return url, hashlib.sha256(raw).hexdigest(), len(raw), dim, ewt


def main():
    pages = {k: text(u) for k, u in SOURCES.items()}
    best = parse_best(pages["tsplib_best"])
    mil = parse_milestones(pages["milestones"])
    names = set(SET) | set(HISTORY_ONLY)
    b99 = parse_bench(pages["concorde99"], names, "nodes_time")
    b03 = parse_bench(pages["concorde03"], names, "time_nodes")
    m = re.search(r"Tour: ([\d,]+) Bound: ([\d,]+)", pages["monalisa"])
    mona_tour, mona_bound = (int(num(m.group(1))), int(num(m.group(2)))) if m else (None, None)

    manifest, rows = [], []
    for name in SET + list(HISTORY_ONLY):
        in_set = name in SET
        if in_set:
            url, sha, size, n, metric = file_info(name)
            manifest.append({"instance": name, "url": url, "sha256": sha, "bytes": size})
        else:
            n, metric = int(re.sub(r"\D", "", name)), HISTORY_ONLY[name]
        r = {"instance": name, "n": n, "metric": metric, "in_set": "yes" if in_set else "no"}
        if name == "mona-lisa100K":
            r.update(value=mona_tour or "", value_kind="best_known_tour", value_source=SOURCES["monalisa"] if mona_tour else "",
                     note=f"best bound {mona_bound} (same source); not proven optimal" if mona_bound else "")
        else:
            v = best.get(name, {})
            val = v.get(metric) or v.get("")
            r.update(value=val or "", value_kind="optimal" if val else "", value_source=SOURCES["tsplib_best"] if val else "")
        if name in mil:
            r.update(year_solved=mil[name][0], solved_by=mil[name][1], history_source=SOURCES["milestones"])
        if name in b99:
            s, nodes, raw = b99[name]
            r.update(concorde99_s="" if s is None else s, concorde99_nodes="" if nodes is None else nodes, concorde99_raw=raw,
                     concorde99_hw=HW99, concorde99_source=SOURCES["concorde99"])
        if name in b03:
            s, nodes, _ = b03[name]
            r.update(concorde03_s=s, concorde03_nodes=nodes, concorde03_hw=HW03, concorde03_source=SOURCES["concorde03"])
        if not in_set:
            r["note"] = f"{metric} metric: not run (the engine reads EUC_2D / CEIL_2D)"
        rows.append(r)

    cols = ["instance", "n", "metric", "in_set", "value", "value_kind", "value_source", "year_solved", "solved_by",
            "history_source", "concorde99_s", "concorde99_nodes", "concorde99_raw", "concorde99_hw", "concorde99_source",
            "concorde03_s", "concorde03_nodes", "concorde03_hw", "concorde03_source", "note"]
    with open(os.path.join(HERE, "reference.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, cols, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, "") for c in cols})
    with open(os.path.join(HERE, "manifest.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, ["instance", "url", "sha256", "bytes"], lineterminator="\n")
        w.writeheader()
        w.writerows(manifest)
    print(f"reference.csv: {len(rows)} rows ({len(manifest)} in the benchmark set); manifest.csv: {len(manifest)} files")
    missing = [r["instance"] for r in rows if not r.get("value")]
    if missing:
        print("no value from the sources:", missing, file=sys.stderr)


if __name__ == "__main__":
    main()
