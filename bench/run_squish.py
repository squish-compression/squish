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
"""Benchmark SQUISH across its compression modes with round-trip verification.

Two modes are measured per corpus file (both produce a one-member archive):

  squish-single  ratio-optimal single-block layout    (c -t 1  /  d)
  squish-mt      multi-threaded block-split layout     (c -t 0  /  d -t 0)

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
SQUISH = os.path.join(ROOT, "squish.exe")
if not os.path.exists(SQUISH):
    SQUISH = os.path.join(ROOT, "squish")

CORPUS = []
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "silesia"))):
    CORPUS.append(("silesia/" + name, os.path.join(ROOT, "corpus", "silesia", name)))
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "canterbury"))):
    CORPUS.append(("canterbury/" + name, os.path.join(ROOT, "corpus", "canterbury", name)))
CORPUS.append(("enwik8", os.path.join(ROOT, "corpus", "enwik8")))


def run(args):
    subprocess.run([SQUISH, "-q"] + args, check=True, stderr=subprocess.DEVNULL)


def bench_stream(src, td, flags, tag):
    """Compress with `c <flags>`, decompress with `d <flags>`, verify."""
    comp = os.path.join(td, tag + ".sqsh")
    rt = os.path.join(td, tag + ".out")
    t0 = time.time(); run(flags + ["c", src, comp]); ct = time.time() - t0
    t0 = time.time(); run(flags + ["d", comp, rt]); dt = time.time() - t0
    ok = filecmp.cmp(src, rt, shallow=False)
    return os.path.getsize(comp), ct, dt, ok


MODES = [
    ("squish-single", lambda src, td: bench_stream(src, td, ["-t", "1"], "single")),
    ("squish-mt",     lambda src, td: bench_stream(src, td, ["-t", "0"], "mt")),
]


def main():
    out_path = os.path.join(ROOT, "bench", "squish.csv")
    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file", "tool", "orig_bytes", "comp_bytes",
                    "compress_s", "decompress_s", "verified"])
        fh.flush()
        for label, src in CORPUS:
            orig = os.path.getsize(src)
            for tool, fn in MODES:
                with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "bench")) as td:
                    comp, ct, dt, ok = fn(src, td)
                w.writerow([label, tool, orig, comp,
                            f"{ct:.2f}", f"{dt:.2f}", "yes" if ok else "NO"])
                fh.flush()
                print(f"{label:24s} {tool:14s} {orig:>10d} -> {comp:>10d}  "
                      f"c={ct:6.1f}s d={dt:6.1f}s verified={ok}", flush=True)
                if not ok:
                    sys.exit(f"ROUND-TRIP FAILURE on {label} ({tool})")
    print("done")


if __name__ == "__main__":
    sys.exit(main())
