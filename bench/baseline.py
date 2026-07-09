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
"""Benchmark baseline compressors (zip -9, bzip2 -9, rar -m5, xz -9e) on the corpus.

Writes CSV rows: file,tool,orig_bytes,comp_bytes,compress_s
Each tool works on a copy in a temp dir so nothing touches the corpus.
"""
import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAR = os.path.join(ROOT, "tools", "rar", "rar")

CORPUS = []
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "silesia"))):
    CORPUS.append(("silesia/" + name, os.path.join(ROOT, "corpus", "silesia", name)))
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "canterbury"))):
    CORPUS.append(("canterbury/" + name, os.path.join(ROOT, "corpus", "canterbury", name)))
CORPUS.append(("enwik8", os.path.join(ROOT, "corpus", "enwik8")))


def bench_zip(src, workdir):
    out = os.path.join(workdir, "out.zip")
    t0 = time.time()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(src, arcname="f")
    dt = time.time() - t0
    return os.path.getsize(out), dt


def bench_bzip2(src, workdir):
    cp = os.path.join(workdir, "f")
    shutil.copy(src, cp)
    t0 = time.time()
    subprocess.run(["bzip2", "-9", cp], check=True)
    dt = time.time() - t0
    return os.path.getsize(cp + ".bz2"), dt


def bench_rar(src, workdir):
    out = os.path.join(workdir, "out.rar")
    t0 = time.time()
    subprocess.run(
        [RAR, "a", "-m5", "-md128m", "-inul", out, src],
        check=True, cwd=workdir,
    )
    dt = time.time() - t0
    return os.path.getsize(out), dt


def bench_xz(src, workdir):
    cp = os.path.join(workdir, "f")
    shutil.copy(src, cp)
    t0 = time.time()
    subprocess.run(["xz", "-9e", "-T", "1", cp], check=True)
    dt = time.time() - t0
    return os.path.getsize(cp + ".xz"), dt


TOOLS = {"zip": bench_zip, "bzip2": bench_bzip2, "rar": bench_rar, "xz": bench_xz}


def main():
    out_csv = os.path.join(ROOT, "bench", "baselines.csv")
    with open(out_csv, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file", "tool", "orig_bytes", "comp_bytes", "compress_s"])
        for label, path in CORPUS:
            orig = os.path.getsize(path)
            for tool, fn in TOOLS.items():
                with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "bench")) as td:
                    comp, dt = fn(path, td)
                w.writerow([label, tool, orig, comp, f"{dt:.2f}"])
                fh.flush()
                print(f"{label:24s} {tool:6s} {orig:>10d} -> {comp:>10d}  {dt:6.1f}s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
