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
YEAR_S = 365.25 * 86400
# Run time of the FIRST optimality proof, where a page states it: (CPU seconds, as stated,
# hardware, source, phrase the source page must contain or None for a non-HTML source).
FIRST_SOLVE = {
    "usa13509": (4 * YEAR_S, "~4 years", "network of many machine types (1998 Concorde)",
                 SOURCES_99 := "https://www.math.uwaterloo.ca/tsp/concorde/benchmarks/bench99.html", "usa13509 9539 ~4 years",
                 "very rough estimate (source note 5); the 1998 paper gives ~10 years for the same run"),
    "d15112": (22.6 * YEAR_S, "~22.6 years", "network of 110 processors at Rice and Princeton (2001 Concorde)", SOURCES_99,
               "d15112 164569 ~22.6 years", ""),
    "pla85900": (136 * YEAR_S, "approximately 136 CPU years", "Xeon / Athlon clusters, scaled to a 2.4 GHz AMD Opteron 250",
                 "https://math.uwaterloo.ca/tsp/pla85900/compute/cpu.htm", "approximately 136 CPU years", "2005-2006"),
}
# Applegate, Bixby, Chvatal, Cook, "On the Solution of Traveling Salesman Problems", Documenta
# Mathematica Extra Volume ICM 1998 III 645-656, table p. 654: the record instances of 1977-1992
# re-solved with their 1998 code on one processor of a Digital AlphaServer 4100 (400 MHz);
# usa13509 on a network of 48 workstations (cumulative CPU estimate).
ABCC98_URL = "https://ems.press/content/book-chapter-files/27276"
ABCC98_HW = "Digital AlphaServer 4100 400 MHz, 1 processor (usa13509: 48 workstations), 1998 code"
ABCC98 = {"gr120": (3.3, "3.3 seconds"), "lin318": (24.6, "24.6 seconds"), "pr1002": (94.7, "94.7 seconds"),
          "gr666": (260.0, "260.0 seconds"), "att532": (294.3, "294.3 seconds"), "pr2392": (342.2, "342.2 seconds"),
          "ts225": (438.9, "438.9 seconds"), "pla7397": (49.5 * 86400, "49.5 days"), "usa13509": (10 * YEAR_S, "∼10 years")}


def abcc98_text():
    """Text of the 1998 paper (pdftotext); None when the tool is missing (values kept, not re-checked)."""
    import shutil
    import subprocess
    import tempfile
    if not shutil.which("pdftotext"):
        return None
    with tempfile.NamedTemporaryFile(suffix=".pdf") as f:
        f.write(get(ABCC98_URL))
        f.flush()
        return re.sub(r"\s+", " ", subprocess.run(["pdftotext", "-layout", f.name, "-"], capture_output=True, text=True).stdout)

# Benchmark set: TSPLIB instances in the engine's metrics (EUC_2D / CEIL_2D) up to ~1000 nodes,
# the larger project references, the solved milestones in these metrics, and mona-lisa100K.
SET = """berlin52 eil51 st70 eil76 pr76 rat99 kroA100 rd100 eil101 lin105 pr107 pr124 bier127 ch130 pr136
pr144 ch150 kroA150 pr152 u159 rat195 d198 kroA200 ts225 tsp225 pr226 pr264 a280 pr299 lin318 rd400 fl417
pr439 pcb442 d493 u574 rat575 p654 d657 u724 rat783 dsj1000 pr1002 pr2392 fl3795 pla7397 usa13509 d15112
pla85900 mona-lisa100K""".split()
# Milestones in other metrics: listed for the history, not run (the engine reads EUC_2D / CEIL_2D).
HISTORY_ONLY = {"dantzig42": "EXPLICIT", "gr120": "EXPLICIT", "att532": "ATT", "gr666": "GEO"}


# Waterloo collections (EUC_2D): National TSPs and VLSI. Each instance has a "Log of Computation"
# page with the date it was solved, the solution time and the machine; the summary page gives the
# best tour / bound. Instances up to SET_MAX_N nodes join the benchmark set.
WATERLOO = {
    "national": ("https://www.math.uwaterloo.ca/tsp/world/", ["countries.html"], r'href="\.?/?([a-z]{2}log\.html)"'),
    "vlsi": ("https://www.math.uwaterloo.ca/tsp/vlsi/", ["index.html"] + [f"page{k}.html" for k in range(2, 12)],
             r'href="\.?/?([a-z]+\d+\.log\.html)"'),
}
SET_MAX_N = 1002
UNIT_S = {"seconds": 1, "minutes": 60, "hours": 3600, "days": 86400, "years": 365.25 * 86400}


def parse_time(said):
    """'560,951 seconds, AMD Athlon 1900+' -> (seconds, '560,951 seconds', 'AMD Athlon 1900+')."""
    m = re.match(r"([\d.,]+)\s*(million\s+)?(seconds|minutes|hours|days|years)\s*,?\s*(.*)", said)
    if not m:
        return None, said, ""
    v = float(m.group(1).replace(",", "")) * (1e6 if m.group(2) else 1) * UNIT_S[m.group(3)]
    return v, said[: m.end(3)].strip(), m.group(4).strip()


def waterloo_rows():
    rows, skipped = [], []
    for coll, (base, pages, link) in WATERLOO.items():
        logs = set()
        for pg in pages:
            logs |= set(re.findall(link, get(base + pg).decode("latin1")))
        summ = text(base + "summary.html")
        best = {}
        for name, gap, bound, tour in re.findall(r"\b([a-z]+\d+) (Optimal|[\d.]+%) ([\d,.]+) ([\d,.]+)", summ):
            best[name] = (gap, int(re.sub(r"[,.]", "", tour)), bound)  # "9.352" (qa194) is a typo for 9,352
        for lg in sorted(logs):
            t = text(base + lg)
            n = int(re.search(r"Number of Cities: ([\d,]+)", t).group(1).replace(",", ""))
            name = next((k for k in best if lg.startswith(k[:2]) and re.sub(r"\D", "", k) == str(n)), None) if coll == "national" else lg.split(".")[0]
            if name is None or name not in best:
                skipped.append(lg)
                continue
            gap, tour, bound = best[name]
            ov = re.search(r"Optimal Value: ([\d,]+)", t)
            if ov and gap == "Optimal" and int(ov.group(1).replace(",", "")) != tour:
                sys.exit(f"{name}: log optimal value {ov.group(1)} != summary {tour}")
            solved = re.search(r"Solved: (\w+ \d+, (\d{4}))", t)
            st = re.search(r"Solution Time: (.+?)(?: History| Notes| Note \d)", t)
            r = {"collection": coll, "instance": name, "n": n, "metric": "EUC_2D",
                 "in_set": "yes" if n <= SET_MAX_N and gap == "Optimal" else "no",
                 "value": tour, "value_kind": "optimal" if gap == "Optimal" else "best_known_tour", "value_source": base + "summary.html"}
            if gap != "Optimal":
                r["note"] = f"open: best bound {bound} (gap {gap})"
            elif n > SET_MAX_N:
                r["note"] = f"not in the campaign set (n > {SET_MAX_N})"
            if solved:
                r.update(year_solved=solved.group(2), solved_on=solved.group(1), solved_by="Concorde (Applegate, Bixby, Chvatal, Cook)" if st else "",
                         history_source=base + lg)
            if st:
                sec, said, hw = parse_time(st.group(1).strip())
                r.update(first_solve_s="" if sec is None else round(sec, 2), first_solve_as_stated=said, first_solve_hw=hw,
                         first_solve_source=base + lg)
            elif solved:
                r["first_solve_note"] = "the log gives the history of bound / tour improvements, not one solution time"
            r["url"] = base + name + ".tsp"
            rows.append(r)
    if skipped:
        print("logs without a summary row:", skipped, file=sys.stderr)
    return rows


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


def file_info(name, url=None):
    url = url or (MONA_URL if name == "mona-lisa100K" else TSPLIB + name + ".tsp.gz")
    raw = get(url)
    if url.endswith(".gz"):
        raw = gzip.decompress(raw)
    head = raw[:4000].decode("latin1")
    dim = int(re.search(r"DIMENSION\s*:\s*(\d+)", head).group(1))
    ewt = re.search(r"EDGE_WEIGHT_TYPE\s*:\s*(\S+)", head).group(1)
    return url, hashlib.sha256(raw).hexdigest(), len(raw), dim, ewt


def main():
    pages = {k: text(u) for k, u in SOURCES.items()}
    t98 = abcc98_text()
    for name, (_, said) in ABCC98.items():
        if t98 is not None and not re.search(re.escape(name) + r"\s+[\d,]+\s+\d[\d,]*\s+nodes?\s+" + re.escape(said), t98):
            sys.exit(f"{name}: '{said}' not found in the 1998 paper")
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
        if name in FIRST_SOLVE:
            sec, said, hw, url, phrase, note = FIRST_SOLVE[name]
            if phrase is not None and phrase not in (pages["concorde99"] if url == SOURCES["concorde99"] else text(url)):
                sys.exit(f"{name}: first-solve time not found on {url}")
            r.update(first_solve_s=round(sec), first_solve_as_stated=said, first_solve_hw=hw, first_solve_source=url,
                     first_solve_note=note)
        if name in ABCC98:
            r.update(abcc98_s=round(ABCC98[name][0], 1), abcc98_as_stated=ABCC98[name][1], abcc98_hw=ABCC98_HW, abcc98_source=ABCC98_URL)
        if name in b99:
            s, nodes, raw = b99[name]
            r.update(concorde99_s="" if s is None else s, concorde99_nodes="" if nodes is None else nodes, concorde99_raw=raw,
                     concorde99_hw=HW99, concorde99_source=SOURCES["concorde99"])
        if name in b03:
            s, nodes, _ = b03[name]
            r.update(concorde03_s=s, concorde03_nodes=nodes, concorde03_hw=HW03, concorde03_source=SOURCES["concorde03"])
        if not in_set:
            r["note"] = f"{metric} metric: not run (the engine reads EUC_2D / CEIL_2D)"
        r["collection"] = "mona_lisa" if name == "mona-lisa100K" else "tsplib"
        rows.append(r)

    for r in waterloo_rows():
        if r["in_set"] == "yes":
            url, sha, size, n, metric = file_info(r["instance"], r["url"])
            if metric != "EUC_2D" or n != r["n"]:
                sys.exit(f"{r['instance']}: file says {metric} n={n}")
            manifest.append({"instance": r["instance"], "url": url, "sha256": sha, "bytes": size})
        rows.append(r)

    cols = ["collection", "instance", "n", "metric", "in_set", "value", "value_kind", "value_source", "year_solved", "solved_on", "solved_by",
            "history_source", "first_solve_s", "first_solve_as_stated", "first_solve_hw", "first_solve_source", "first_solve_note",
            "abcc98_s", "abcc98_as_stated", "abcc98_hw", "abcc98_source",
            "concorde99_s", "concorde99_nodes", "concorde99_raw", "concorde99_hw", "concorde99_source",
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
