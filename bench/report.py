#!/usr/bin/env python3
# Copyright (C) 2026  Paige Julianne Sullivan
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Merge baselines.csv + squish.csv into comparison tables (markdown).

Two tables are emitted:
  1. SQUISH (ratio-optimal single-block) versus zip/bzip2/rar/xz.
  2. SQUISH's own modes head to head: single-block versus multi-threaded
     block-split (size, compress/decompress time, speedup).
"""
import csv
import os
import sys
from collections import defaultdict

# The tables use "Δ" and em-dashes; keep them intact on Windows' cp1252 stdout.
sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RIVAL_ORDER = ["zip", "bzip2", "rar", "xz"]
RIVAL_LABEL = {"zip": "zip -9", "bzip2": "bzip2 -9", "rar": "rar -m5", "xz": "xz -9e"}
MODES = ["squish-single", "squish-mt"]

# file -> tool -> (orig, comp, compress_s, decompress_s)
data = defaultdict(dict)
for path in ("baselines.csv", "squish.csv"):
    with open(os.path.join(ROOT, "bench", path)) as fh:
        for row in csv.DictReader(fh):
            data[row["file"]][row["tool"]] = (
                int(row["orig_bytes"]), int(row["comp_bytes"]),
                float(row["compress_s"]),
                float(row.get("decompress_s") or 0.0))

# Only report rivals actually present in the CSVs — rar is skipped where no rar
# executable exists (e.g. Windows), and the tables adapt instead of going blank.
RIVALS = [r for r in RIVAL_ORDER if any(r in v for v in data.values())]
NAMED = [r for r in RIVALS if r != "xz"]   # "named" rivals: everything but xz


def order(files):
    return sorted(files, key=lambda f: -data[f]["squish-single"][0])


# ---------------------------------------------------------------------------
# Table 1: SQUISH vs the field
# ---------------------------------------------------------------------------
cols = RIVALS + ["squish-single"]
files = order([f for f in data if all(t in data[f] for t in cols)])

print("### SQUISH vs " + " / ".join(RIVALS) + "\n")
print("Ratio-optimal single-block SQUISH against each rival's strongest "
      "setting. zip/bzip2/xz are measured with Python's zlib/bz2/lzma so the "
      "suite runs anywhere; rar (no stdlib) is measured with the rar CLI where "
      "available, and its deterministic sizes carry over otherwise.\n")
print("| file | orig | " + " | ".join(RIVAL_LABEL[r] for r in RIVALS) +
      " | SQUISH | vs best rival |")
print("|---|---:|" + "---:|" * (len(RIVALS) + 2))
tot = defaultdict(int); orig_tot = 0
wins_all = wins_named = 0
for f in files:
    orig = data[f]["squish-single"][0]; orig_tot += orig
    sizes = {t: data[f][t][1] for t in cols}
    for t in cols: tot[t] += sizes[t]
    best_rival = min(sizes[t] for t in RIVALS)
    named_rival = min((sizes[t] for t in NAMED), default=best_rival)
    sq = sizes["squish-single"]
    if sq < best_rival: wins_all += 1
    if sq < named_rival: wins_named += 1
    delta = 100.0 * (best_rival - sq) / best_rival
    row = [f, f"{orig:,}"]
    for t in cols:
        s = f"{sizes[t]:,}"
        if sizes[t] == min(sizes.values()): s = f"**{s}**"
        row.append(s)
    row.append(f"{delta:+.1f}%")
    print("| " + " | ".join(row) + " |")

row = ["**TOTAL**", f"{orig_tot:,}"]
best_tot = min(tot.values())
for t in cols:
    s = f"{tot[t]:,}"
    if tot[t] == best_tot: s = f"**{s}**"
    row.append(s)
best_rival_tot = min(tot[t] for t in RIVALS)
row.append(f"{100.0*(best_rival_tot-tot['squish-single'])/best_rival_tot:+.1f}%")
print("| " + " | ".join(row) + " |")

n = len(files)
print()
print(f"SQUISH beats {'+'.join(NAMED)} on {wins_named}/{n} files; "
      f"beats all {len(RIVALS)} (incl. xz -9e) on {wins_all}/{n}.")
for t in cols:
    label = "squish" if t == "squish-single" else t
    print(f"  {label:7s} total {tot[t]:>12,}  ratio {tot[t]/orig_tot:.4f}")

# ---------------------------------------------------------------------------
# Table 2: SQUISH modes head to head
# ---------------------------------------------------------------------------
mfiles = order([f for f in data if all(t in data[f] for t in MODES)])

print()
print("### SQUISH compression modes\n")
print("Same corpus, two ways to run SQUISH. `single` is the ratio-optimal "
      "single-block layout; `mt` splits each member into blocks across all "
      "cores (a little ratio for a lot of speed). Both write a one-member "
      "SQUISH archive.\n")
print("| file | orig | single | c/d s | mt | c/d s | mt Δsize | mt speedup |")
print("|---|---:|---:|---:|---:|---:|---:|---:|")
mtot = defaultdict(int); morig = 0
ct_tot = defaultdict(float); dt_tot = defaultdict(float)
for f in mfiles:
    o, s_sz, s_ct, s_dt = data[f]["squish-single"]
    _, m_sz, m_ct, m_dt = data[f]["squish-mt"]
    morig += o
    mtot["single"] += s_sz; mtot["mt"] += m_sz
    ct_tot["single"] += s_ct; ct_tot["mt"] += m_ct
    dt_tot["single"] += s_dt; dt_tot["mt"] += m_dt
    dsize = 100.0 * (m_sz - s_sz) / s_sz
    speed = s_ct / m_ct if m_ct else 0.0
    print("| " + " | ".join([
        f, f"{o:,}",
        f"{s_sz:,}", f"{s_ct:.1f}/{s_dt:.1f}",
        f"{m_sz:,}", f"{m_ct:.1f}/{m_dt:.1f}",
        f"{dsize:+.1f}%", f"{speed:.2f}x",
    ]) + " |")

dsize_tot = 100.0 * (mtot["mt"] - mtot["single"]) / mtot["single"]
speed_tot = ct_tot["single"] / ct_tot["mt"] if ct_tot["mt"] else 0.0
print("| " + " | ".join([
    "**TOTAL**", f"{morig:,}",
    f"**{mtot['single']:,}**", f"{ct_tot['single']:.0f}/{dt_tot['single']:.0f}",
    f"{mtot['mt']:,}", f"{ct_tot['mt']:.0f}/{dt_tot['mt']:.0f}",
    f"{dsize_tot:+.1f}%", f"{speed_tot:.2f}x",
]) + " |")
print()
print(f"  single  {mtot['single']:>12,}  ratio {mtot['single']/morig:.4f}  "
      f"({ct_tot['single']:.0f}s compress)")
print(f"  mt      {mtot['mt']:>12,}  ratio {mtot['mt']/morig:.4f}  "
      f"({ct_tot['mt']:.0f}s compress, {speed_tot:.2f}x faster, {dsize_tot:+.1f}% size)")
