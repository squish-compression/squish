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

Runs on Linux, macOS, and Windows: zip/bzip2/xz use Python's own zlib/bz2/lzma
(the same libraries the CLI tools wrap, so the compressed sizes match). rar has
no standard library, so it is measured only when a rar executable is found —
bundled at tools/rar/rar[.exe] or on PATH — and skipped with a note otherwise.

Writes CSV rows: file,tool,orig_bytes,comp_bytes,compress_s
"""
import bz2
import csv
import lzma
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_rar():
    """Locate a rar executable runnable on this platform: the bundled copy
    first, then PATH. The bundled tools/rar/rar is a Linux binary, so it is
    only considered off Windows; Windows looks for rar.exe on PATH."""
    bundled = os.path.join(ROOT, "tools", "rar")
    if os.name == "nt":
        cands = [os.path.join(bundled, "rar.exe")]
    else:
        cands = [os.path.join(bundled, "rar")]
    for name in ("rar", "WinRAR"):
        found = shutil.which(name)
        if found:
            cands.append(found)
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


RAR = find_rar()

CORPUS = []
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "silesia"))):
    CORPUS.append(("silesia/" + name, os.path.join(ROOT, "corpus", "silesia", name)))
for name in sorted(os.listdir(os.path.join(ROOT, "corpus", "canterbury"))):
    CORPUS.append(("canterbury/" + name, os.path.join(ROOT, "corpus", "canterbury", name)))
CORPUS.append(("enwik8", os.path.join(ROOT, "corpus", "enwik8")))


def read_bytes(src):
    with open(src, "rb") as fh:
        return fh.read()


def bench_zip(src, workdir):
    """zip -9 (DEFLATE, level 9) via Python's zlib."""
    out = os.path.join(workdir, "out.zip")
    t0 = time.time()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(src, arcname="f")
    dt = time.time() - t0
    return os.path.getsize(out), dt


def bench_bzip2(src, workdir):
    """bzip2 -9 via Python's bz2 (same libbzip2, so the size matches)."""
    data = read_bytes(src)
    t0 = time.time()
    comp = bz2.compress(data, 9)
    return len(comp), time.time() - t0


def bench_xz(src, workdir):
    """xz -9e via Python's lzma (XZ container, preset 9 + extreme)."""
    data = read_bytes(src)
    t0 = time.time()
    comp = lzma.compress(data, format=lzma.FORMAT_XZ,
                         preset=9 | lzma.PRESET_EXTREME)
    return len(comp), time.time() - t0


def bench_rar(src, workdir):
    """rar -m5 (max compression). Needs a rar executable (see find_rar)."""
    out = os.path.join(workdir, "out.rar")
    t0 = time.time()
    subprocess.run(
        [RAR, "a", "-m5", "-md128m", "-inul", out, src],
        check=True, cwd=workdir,
    )
    dt = time.time() - t0
    return os.path.getsize(out), dt


def main():
    tools = {"zip": bench_zip, "bzip2": bench_bzip2, "xz": bench_xz}
    if RAR:
        tools["rar"] = bench_rar
    else:
        print("note: no rar executable found (tools/rar/rar[.exe] or PATH); "
              "skipping rar", file=sys.stderr)

    out_csv = os.path.join(ROOT, "bench", "baselines.csv")
    with open(out_csv, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file", "tool", "orig_bytes", "comp_bytes", "compress_s"])
        for label, path in CORPUS:
            orig = os.path.getsize(path)
            for tool, fn in tools.items():
                with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "bench")) as td:
                    comp, dt = fn(path, td)
                w.writerow([label, tool, orig, comp, f"{dt:.2f}"])
                fh.flush()
                print(f"{label:24s} {tool:6s} {orig:>10d} -> {comp:>10d}  {dt:6.1f}s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
