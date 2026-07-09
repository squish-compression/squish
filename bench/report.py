#!/usr/bin/env python3
"""Merge baselines.csv + squish.csv into a comparison table (markdown)."""
import csv
import os
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = ["zip", "bzip2", "rar", "xz", "squish"]

data = defaultdict(dict)   # file -> tool -> (orig, comp, ct)
for path in ("baselines.csv", "squish.csv"):
    with open(os.path.join(ROOT, "bench", path)) as fh:
        for row in csv.DictReader(fh):
            data[row["file"]][row["tool"]] = (
                int(row["orig_bytes"]), int(row["comp_bytes"]),
                float(row["compress_s"]))

files = [f for f in data if all(t in data[f] for t in TOOLS)]
files.sort(key=lambda f: -data[f]["zip"][0])

print("| file | orig | zip -9 | bzip2 -9 | rar -m5 | xz -9e | SQUISH | vs best rival |")
print("|---|---:|---:|---:|---:|---:|---:|---:|")
tot = defaultdict(int); orig_tot = 0
wins_all = wins_named = 0
for f in files:
    orig = data[f]["zip"][0]; orig_tot += orig
    sizes = {t: data[f][t][1] for t in TOOLS}
    for t in TOOLS: tot[t] += sizes[t]
    best_rival = min(sizes[t] for t in ("zip", "bzip2", "rar", "xz"))
    named_rival = min(sizes[t] for t in ("zip", "bzip2", "rar"))
    sq = sizes["squish"]
    if sq < best_rival: wins_all += 1
    if sq < named_rival: wins_named += 1
    delta = 100.0 * (best_rival - sq) / best_rival
    row = [f, f"{orig:,}"]
    for t in TOOLS:
        s = f"{sizes[t]:,}"
        if sizes[t] == min(sizes.values()): s = f"**{s}**"
        row.append(s)
    row.append(f"{delta:+.1f}%")
    print("| " + " | ".join(row) + " |")

row = ["**TOTAL**", f"{orig_tot:,}"]
best_tot = min(tot.values())
for t in TOOLS:
    s = f"{tot[t]:,}"
    if tot[t] == best_tot: s = f"**{s}**"
    row.append(s)
row.append(f"{100.0*(min(tot[t] for t in ('zip','bzip2','rar','xz'))-tot['squish'])/min(tot[t] for t in ('zip','bzip2','rar','xz')):+.1f}%")
print("| " + " | ".join(row) + " |")

n = len(files)
print()
print(f"SQUISH beats zip+bzip2+rar on {wins_named}/{n} files; "
      f"beats all four (incl. xz -9e) on {wins_all}/{n}.")
for t in TOOLS:
    print(f"  {t:7s} total {tot[t]:>12,}  ratio {tot[t]/orig_tot:.4f}")
