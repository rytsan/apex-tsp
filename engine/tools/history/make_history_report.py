#!/usr/bin/env python3
"""Historical comparison report (Sprint 6 M6.7) from a campaign folder (GUI Benchmarks tab or
run_history.py) + reference.csv:

  python3 engine/tools/history/make_history_report.py out/history/final [docs/HISTORY_REPORT.md]

Writes the Markdown report, docs/history/*.svg charts and docs/history/history.json (the rows the
GUI shows). Gaps: certified = (UB - LB) / UB with the solver's own LB; vs reference = (UB - ref) /
ref. mona-lisa100K is compared with its best known tour and never called optimal.
"""
import csv
import json
import math
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SOLVERS = ["apex_exact", "apex_scalable", "concorde", "lkh"]
NAMES = {"apex_exact": "APEX exact", "apex_scalable": "APEX scalable", "concorde": "Concorde", "lkh": "LKH-3"}
COLORS = {"apex_exact": "#1A73E8", "apex_scalable": "#E8710A", "concorde": "#1E8E3E", "lkh": "#8E3FD0", "concorde03": "#9AA1AC"}


def fnum(v, fmt="{:.1f}"):
    return "-" if v is None or (isinstance(v, float) and math.isnan(v)) else fmt.format(v)


def load(campaign):
    ref = {r["instance"]: r for r in csv.DictReader(open(os.path.join(HERE, "reference.csv")))}
    runs = {}
    for f in os.listdir(campaign):
        if f.endswith(".json") and "__" in f:
            inst, solver = f[:-5].split("__", 1)
            runs.setdefault(inst, {})[solver] = json.load(open(os.path.join(campaign, f)))
    meta = json.load(open(os.path.join(campaign, "meta.json"))) if os.path.exists(os.path.join(campaign, "meta.json")) else {}
    return ref, runs, meta


def gap_ref(run, refv):
    ub = run.get("ub") if run else None
    return None if ub is None or not refv else 100.0 * (ub - refv) / refv


def gap_cert(run):
    if not run or run.get("ub") is None or run.get("lb") is None:
        return None
    return 100.0 * (run["ub"] - run["lb"]) / run["ub"]


def svg_scatter(path, title, series, xlabel, ylabel, logy=True, logx=True):
    """series: [(label, color, [(x, y), ...])] -> small dependency-free SVG chart."""
    W, H, L, R, T, B = 720, 420, 70, 150, 36, 46
    pts = [p for _, _, s in series for p in s if p[0] and p[1] and p[1] > 0]
    if not pts:
        return False
    fx = (lambda v: math.log10(v)) if logx else (lambda v: v)
    fy = (lambda v: math.log10(v)) if logy else (lambda v: v)
    x0, x1 = min(fx(p[0]) for p in pts), max(fx(p[0]) for p in pts)
    y0, y1 = min(fy(p[1]) for p in pts), max(fy(p[1]) for p in pts)
    if x1 - x0 < 1e-9: x1 += 1
    if y1 - y0 < 1e-9: y1 += 1
    X = lambda v: L + (W - L - R) * (fx(v) - x0) / (x1 - x0)
    Y = lambda v: H - B - (H - T - B) * (fy(v) - y0) / (y1 - y0)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="#ffffff"/>', f'<text x="{L}" y="22" font-size="14" font-weight="600">{title}</text>',
         f'<rect x="{L}" y="{T}" width="{W - L - R}" height="{H - T - B}" fill="none" stroke="#d0d4da"/>']
    for k in range(math.floor(y0), math.ceil(y1) + 1) if logy else []:
        if y0 <= k <= y1:
            y = H - B - (H - T - B) * (k - y0) / (y1 - y0)
            o.append(f'<line x1="{L}" x2="{W - R}" y1="{y:.1f}" y2="{y:.1f}" stroke="#eef0f3"/><text x="{L - 6}" y="{y + 4:.1f}" text-anchor="end" fill="#5f6773">{10 ** k:g}</text>')
    for k in range(math.floor(x0), math.ceil(x1) + 1) if logx else []:
        if x0 <= k <= x1:
            x = L + (W - L - R) * (k - x0) / (x1 - x0)
            o.append(f'<text x="{x:.1f}" y="{H - B + 16}" text-anchor="middle" fill="#5f6773">{10 ** k:g}</text>')
    o.append(f'<text x="{(L + W - R) / 2}" y="{H - 8}" text-anchor="middle" fill="#5f6773">{xlabel}</text>')
    o.append(f'<text x="14" y="{(T + H - B) / 2}" transform="rotate(-90 14 {(T + H - B) / 2})" text-anchor="middle" fill="#5f6773">{ylabel}</text>')
    for i, (label, color, s) in enumerate(series):
        for x, y in s:
            if x and y and y > 0:
                o.append(f'<circle cx="{X(x):.1f}" cy="{Y(y):.1f}" r="3.5" fill="{color}" fill-opacity="0.8"/>')
        o.append(f'<circle cx="{W - R + 16}" cy="{T + 10 + 20 * i}" r="5" fill="{color}"/><text x="{W - R + 26}" y="{T + 14 + 20 * i}">{label}</text>')
    o.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(o))
    return True


def main():
    campaign = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "history", "final")
    out_md = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "HISTORY_REPORT.md")
    figdir = os.path.join(os.path.dirname(out_md), "history")
    os.makedirs(figdir, exist_ok=True)
    ref, runs, meta = load(campaign)
    rows = sorted((r for r in ref.values() if r["in_set"] == "yes"), key=lambda r: int(r["n"]))
    table, data = [], []
    for r in rows:
        name, n, refv = r["instance"], int(r["n"]), int(r["value"])
        rr = runs.get(name, {})
        ex, sc, co, lk = (rr.get(s) for s in SOLVERS)
        ratio = None
        if ex and co and ex.get("status") == "OPTIMAL_PROVEN" and co.get("status") == "OPTIMAL_PROVEN":
            ratio = ex["wall_s"] / max(co["wall_s"], 1e-3)
        data.append({"instance": name, "n": n, "reference": refv, "reference_kind": r["value_kind"],
                     "year_solved": r["year_solved"], "solved_by": r["solved_by"],
                     "concorde99": r["concorde99_raw"], "concorde03": r["concorde03_s"], "runs": rr, "exact_vs_concorde": ratio})

        def exact(run):
            if not run:
                return "-"
            if run.get("status") == "OPTIMAL_PROVEN":
                return f"**proven** {fnum(run.get('wall_s'))} s"
            g = gap_cert(run)
            return f"{run.get('status')} {fnum(g, '{:.3f}')}%" if g is not None else str(run.get("status"))

        def heur(run):
            if not run:
                return "-"
            g = gap_ref(run, refv)
            return f"{fnum(g, '{:+.3f}')}% ({fnum(run.get('wall_s'))} s)" if g is not None else str(run.get("status"))

        kind = "best known tour" if r["value_kind"] == "best_known_tour" else "opt"
        hist = f"{r['year_solved']} {r['solved_by']}" if r["year_solved"] else ""
        if r.get("first_solve_as_stated"):
            hist += f" ({r['first_solve_as_stated']} CPU)"
        def ratio_to(sec):
            return fnum(ex["wall_s"] / float(sec), "{:.2g}x") if ex and ex.get("status") == "OPTIMAL_PROVEN" and sec else "-"
        vs_pub = f"{ratio_to(r.get('abcc98_s'))} / {ratio_to(r['concorde03_s'])} / {ratio_to(r.get('first_solve_s'))}"
        table.append(f"| {name} | {n} | {refv} ({kind}) | {hist} | {r.get('abcc98_as_stated') or '-'} | {r['concorde99_raw'] or '-'} | {r['concorde03_s'] or '-'} | "
                     f"{exact(ex)} | {heur(sc)} | {exact(co)} | {heur(lk)} | {fnum(ratio, '{:.1f}x')} | {vs_pub} |")
    json.dump({"meta": meta, "rows": data}, open(os.path.join(figdir, "history.json"), "w"), indent=1)

    def series(solver, f):
        return [(d["n"], f(d["runs"][solver])) for d in data if solver in d["runs"] and f(d["runs"][solver]) is not None]

    proven_t = lambda run: run["wall_s"] if run.get("status") == "OPTIMAL_PROVEN" else None

    def shown_gap(run):  # log axis: a gap of 0 is drawn at 1e-4 %
        g = gap_ref(run, int(ref[run["instance"]]["value"]))
        return None if g is None else max(g, 1e-4)
    have_time = svg_scatter(os.path.join(figdir, "time_vs_n.svg"), "Time to proven optimum (this machine; Concorde 2003 published)",
                            [("APEX exact", COLORS["apex_exact"], series("apex_exact", proven_t)),
                             ("Concorde (here)", COLORS["concorde"], series("concorde", proven_t)),
                             ("Concorde 2003 (Xeon 2.8 GHz)", COLORS["concorde03"],
                              [(d["n"], float(d["concorde03"])) for d in data if d["concorde03"]])],
                            "nodes", "seconds")
    have_gap = svg_scatter(os.path.join(figdir, "gap_vs_n.svg"), "Heuristic gap vs reference (time box)",
                           [("APEX scalable", COLORS["apex_scalable"], series("apex_scalable", shown_gap)),
                            ("LKH-3", COLORS["lkh"], series("lkh", shown_gap))],
                           "nodes", "gap % (0 shown at 1e-4)")

    def stats(solver):
        rs = [d["runs"][solver] for d in data if solver in d["runs"]]
        proven = sum(1 for x in rs if x.get("status") == "OPTIMAL_PROVEN")
        gaps = [gap_ref(x, d["reference"]) for d in data for s, x in d["runs"].items() if s == solver and x.get("ub") is not None]
        at_ref = sum(1 for g in gaps if g is not None and abs(g) < 1e-12)
        return len(rs), proven, at_ref, (statistics.median(gaps) if gaps else None)

    ratios = [d["exact_vs_concorde"] for d in data if d["exact_vs_concorde"]]
    geo = math.exp(sum(math.log(x) for x in ratios) / len(ratios)) if ratios else None
    lines = ["# APEX-TSP — Historical benchmark (Sprint 6)", "",
             f"Campaign `{os.path.relpath(campaign, ROOT)}`; generated by `engine/tools/history/make_history_report.py`. "
             "Reference values, history and published Concorde times come from `engine/tools/history/reference.csv`, "
             "where every field names its source page.", "",
             "| context | value |", "|---|---|",
             f"| date | {meta.get('date', '-')} |", f"| hardware | {meta.get('cpu', '-')}, {meta.get('logical_cpus', '-')} logical CPUs |",
             f"| OS | {meta.get('os', '-')} |", f"| APEX | {meta.get('threads_apex', 5)} threads, `--mem-limit-mb {meta.get('mem_limit_mb', 4096)}`, seed {meta.get('seed', 1)} |",
             "| Concorde / LKH-3 | 1 thread, seed 1; test environment only (not part of the product) |",
             f"| baselines | {(meta.get('baselines') or '-').strip().replace(chr(10), '; ')} |", "",
             "Time boxes per run: exact (APEX exact, Concorde): 60 s below 200 nodes, 600 s below 1000, 1800 s below 3000, 3600 s up to "
             f"{meta.get('exact_max_n', 4000)} nodes (larger: not run); heuristic (APEX scalable, LKH-3): 30 s below 1000 nodes, 120 s below 5000, 600 s. "
             "APEX scalable always uses its whole time box; LKH-3 stops at its own trial limit.", "",
             "## Summary", "", "| solver | runs | proven optimal | at the reference value | median gap vs reference |", "|---|---|---|---|---|"]
    for s in SOLVERS:
        nr, pr, at, med = stats(s)
        lines.append(f"| {NAMES[s]} | {nr} | {pr if s in ('apex_exact', 'concorde') else '- (heuristic)'} | {at} | {fnum(med, '{:+.4f}')}% |")
    lines += ["", f"APEX exact / Concorde time on instances both proved: {len(ratios)} instances, geometric mean {fnum(geo, '{:.1f}')}x"
              f"{', median ' + fnum(statistics.median(ratios), '{:.1f}') + 'x' if ratios else ''}.", ""]
    if have_time:
        lines += ["![time vs n](history/time_vs_n.svg)", ""]
    if have_gap:
        lines += ["![gap vs n](history/gap_vs_n.svg)", ""]
    lines += ["## Per instance", "",
              "| instance | n | reference | first solved | Concorde 1998 paper (400 MHz Alpha) | Concorde 1999 s (500 MHz Alpha) | Concorde 2003 s (2.8 GHz Xeon) | APEX exact | APEX scalable vs ref | Concorde here | LKH-3 vs ref | APEX / Concorde here | APEX / 1998 / 2003 / first |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|"] + table
    lines += ["", "## Milestones: then and now", "",
              "| year | team | instance | n | first solution CPU (published) | Concorde 1998 paper | Concorde 1999 | Concorde here | APEX here |", "|---|---|---|---|---|---|---|---|---|"]
    for r in sorted((r for r in ref.values() if r["year_solved"]), key=lambda r: int(r["year_solved"])):
        rr = runs.get(r["instance"], {})
        now_c = rr.get("concorde")
        best_apex = rr.get("apex_exact") or rr.get("apex_scalable")
        if r["in_set"] != "yes":
            c, a = "-", f"not run ({r['metric']} metric)"
        else:
            c = (f"proven {fnum(now_c['wall_s'])} s" if now_c and now_c.get("status") == "OPTIMAL_PROVEN" else (now_c or {}).get("status", "not run"))
            a = "-"
            if best_apex:
                a = (f"proven {fnum(best_apex['wall_s'])} s" if best_apex.get("status") == "OPTIMAL_PROVEN"
                     else f"UB {fnum(gap_ref(best_apex, int(r['value'])), '{:+.3f}')}% vs opt in {fnum(best_apex.get('wall_s'))} s")
        first = f"{r['first_solve_as_stated']} ({r['first_solve_hw']})" if r.get("first_solve_as_stated") else "not in the sources"
        lines.append(f"| {r['year_solved']} | {r['solved_by']} | {r['instance']} | {r['n']} | {first} | {r.get('abcc98_as_stated') or '-'} | {r['concorde99_raw'] or '-'} | {c} | {a} |")
    lines += ["", "mona-lisa100K: compared with its best known tour 5757191 (best bound 5757084); no tour of it is called optimal here.", ""]
    with open(out_md, "w") as f:
        f.write("\n".join(lines))
    print(out_md)


if __name__ == "__main__":
    main()
