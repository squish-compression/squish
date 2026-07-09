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
"""Benchmark SQUISH on the corpus with round-trip verification.

Writes CSV rows: file,tool,orig_bytes,comp_bytes,compress_s,decompress_s,verified
"""
import csv
import filecmp
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SQUISH = os.path.join(ROOT, "squish")

CORPUS = []
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "silesia"))):
    CORPUS.append(("silesia/" + name, os.path.join(ROOT, "corpus", "silesia", name)))
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "canterbury"))):
    CORPUS.append(("canterbury/" + name, os.path.join(ROOT, "corpus", "canterbury", name)))
CORPUS.append(("enwik8", os.path.join(ROOT, "corpus", "enwik8")))

out_path = os.path.join(ROOT, "bench", "squish.csv")
with open(out_path, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["file", "tool", "orig_bytes", "comp_bytes",
                "compress_s", "decompress_s", "verified"])
    fh.flush()
    for label, src in CORPUS:
        with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "bench")) as td:
            sq = os.path.join(td, "f.sq")
            rt = os.path.join(td, "f.out")
            t0 = time.time()
            subprocess.run([SQUISH, "c", src, sq], check=True,
                           stderr=subprocess.DEVNULL)
            ct = time.time() - t0
            t0 = time.time()
            subprocess.run([SQUISH, "d", sq, rt], check=True,
                           stderr=subprocess.DEVNULL)
            dt = time.time() - t0
            ok = filecmp.cmp(src, rt, shallow=False)
            w.writerow([label, "squish", os.path.getsize(src),
                        os.path.getsize(sq), f"{ct:.2f}", f"{dt:.2f}",
                        "yes" if ok else "NO"])
            fh.flush()
            print(f"{label}: {os.path.getsize(src)} -> {os.path.getsize(sq)} "
                  f"c={ct:.1f}s d={dt:.1f}s verified={ok}", flush=True)
            if not ok:
                sys.exit(f"ROUND-TRIP FAILURE on {label}")
print("done")
