#!/usr/bin/env python3
"""Checks reference.csv / manifest.csv (G6.5): every value is sourced, fields come in sourced
groups, the set matches the manifest, values agree with the optimal tours shipped in data/.

  python3 engine/tools/history/test_reference.py
"""
import csv
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ref = list(csv.DictReader(open(os.path.join(HERE, "reference.csv"))))
man = {r["instance"]: r for r in csv.DictReader(open(os.path.join(HERE, "manifest.csv")))}

GROUPS = {  # field -> the source field that must be present when the field is
    "value": "value_source", "year_solved": "history_source", "solved_by": "history_source",
    "concorde99_raw": "concorde99_source", "concorde99_hw": "concorde99_source",
    "concorde03_s": "concorde03_source", "concorde03_hw": "concorde03_source",
}
names = [r["instance"] for r in ref]
assert len(names) == len(set(names)), "duplicate instance"
for r in ref:
    assert r["value"] and int(r["value"]) > 0, f"{r['instance']}: no value"
    assert r["value_kind"] in ("optimal", "best_known_tour"), r["instance"]
    for field, src in GROUPS.items():
        if r[field]:
            assert r[src].startswith("https://"), f"{r['instance']}: {field} without {src}"
    if r["in_set"] == "yes":
        assert r["instance"] in man, f"{r['instance']} missing from manifest"
        assert r["metric"] in ("EUC_2D", "CEIL_2D"), f"{r['instance']}: metric {r['metric']} in the run set"
        assert re.fullmatch(r"[0-9a-f]{64}", man[r["instance"]]["sha256"])
    else:
        assert r["instance"] not in man and r["note"], r["instance"]
assert set(man) == {r["instance"] for r in ref if r["in_set"] == "yes"}
assert [r for r in ref if r["instance"] == "mona-lisa100K"][0]["value_kind"] == "best_known_tour"  # never "optimal"
checked = 0
for f in sorted(os.listdir(os.path.join(ROOT, "data"))):
    if not f.endswith(".opt.tour"):
        continue
    head = open(os.path.join(ROOT, "data", f)).read(400)
    m = re.search(r"COMMENT\s*:.*\((\d+)\)", head)
    row = [r for r in ref if r["instance"] == f[: -len(".opt.tour")]]
    if m and row:
        assert int(m.group(1)) == int(row[0]["value"]), f"{f}: comment {m.group(1)} != reference {row[0]['value']}"
        checked += 1
print(f"reference.csv OK: {len(ref)} rows, {len(man)} pinned files, {checked} values cross-checked with .opt.tour comments")
