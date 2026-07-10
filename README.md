# SQUISH

A context-mixing file compressor that outcompresses `zip -9`, `bzip2 -9`, and
`rar -m5` on 23 of 24 standard-corpus files (and `xz -9e` on 19 of 24),
shipped as a linkable C library (`libsquish.so` / `squish.dll`) with a
command-line tool.

SQUISH treats compression as prediction: ten statistical models estimate the
probability of every bit, an online-trained logistic mixer fuses their votes,
and an arithmetic coder turns the probabilities into bits. The decompressor
runs the identical models in lockstep, so the format needs no dictionaries,
tables, or block structure. See [SQUISH.md](SQUISH.md) for the algorithm.

## Results

Silesia + Canterbury + enwik8 (315 MB, 24 files), every file round-trip
verified. Full table in [bench/RESULTS.md](bench/RESULTS.md).

| tool | total compressed | ratio |
|---|---:|---:|
| zip -9 | 104,849,141 | 0.333 |
| bzip2 -9 | 84,058,237 | 0.267 |
| rar -m5 | 80,560,956 | 0.256 |
| xz -9e | 73,780,732 | 0.234 |
| **SQUISH** | **67,430,251** | **0.214** |

The trade: ~0.6 MB/s, symmetric (decompression costs the same as
compression), and ~150 MB of model memory per stream. SQUISH spends CPU that
zip/bzip2/rar don't and buys ratio with it.

## Build

```sh
make            # libsquish.so + squish CLI
make test       # build and run the test suite
make install    # headers, libs, CLI, pkg-config file to /usr/local
make dll        # squish.dll + squish.exe via mingw-w64 cross compiler, if installed
make windows-dll   # squish.dll + squish.exe via MSVC (cl.exe on PATH), native Windows build
```

On Windows without make, run `build-windows.bat` — it locates Visual
Studio automatically and builds `squish.dll` + `squish.exe`.

No dependencies beyond libc/libm.

## CLI

```sh
./squish c bigfile bigfile.sq          # compress
./squish d bigfile.sq restored         # decompress (checksum-verified)
./squish c project project.sqar        # pack a whole directory tree
./squish l project.sqar                # list the archive's contents
./squish x project.sqar src/main.c     # extract one file (or a subtree)
./squish d project.sqar restored-dir   # recreate the whole tree under restored-dir
./squish -t 0 c bigfile bigfile.sq     # compress on all cores (multi-block)
./squish -t 0 -b 4 c big big.sq        # ... with 4 MiB blocks (more parallel,
                                       #     slightly worse ratio)
./squish s report.pdf report.run       # make a self-extracting archive
./report.run                           # ... run it to restore report.pdf
```

`input` may be a file or a **directory**. A directory is packed into a
**seekable `SQAR` archive** — every file compressed as its own stream, with
directories, empty directories, and permission bits preserved. That lets `l`
list the contents and `x` pull out a single file or subtree by seeking to just
its stream, without inflating the rest; `d` recreates the whole tree under
`output`. Extraction refuses absolute paths and `..`, so an archive never
writes outside its target directory, and members are stored sorted, so the
output depends only on the tree. The tradeoff for random access: each file's
model starts cold, so many tiny files compress a little worse than one solid
stream. Single files are compressed exactly as before. Library consumers get
the same operations through the `squish_archive_*` API; see
[docs/API.md](docs/API.md) and [docs/FORMAT.md §12](docs/FORMAT.md).

`-t N` compresses with N threads (`0` = all cores) by splitting the input
into independently modeled blocks — near-linear speedup, at a small ratio
cost because each block's model starts cold (about 1–2% at the default
16 MiB blocks, more at smaller `-b` sizes). The default `-t 1` keeps the
ratio-optimal single-block format used for the results table above.
Decompression always uses all cores when the stream allows it (`-t` caps
it) and reads both formats transparently. Budget ~150 MB of model memory
per thread.

The `s` command writes a **self-extracting archive**: `squish s input output`
compresses `input` (a file or a directory tree) and produces an executable
`output` that carries the compressed data. Running it — with no `squish` and
no `libsquish` installed — decompresses (and checksum-verifies) the original
back to its stored name in the current directory, or to a path you name;
a directory archive unpacks the whole tree:

```sh
./squish s report.pdf report.run   # build (Linux: chmod +x is applied for you)
./report.run                       # restore report.pdf here
./report.run -f other.pdf          # ... or to a chosen path (-f to overwrite)
```

The archive is this CLI used as a stub with the payload appended, so it is
platform-specific: build it on Linux for a Linux archive, on Windows for a
Windows one (name the output `*.exe`). It takes the same `-t`/`-b`/`-q`
options as `c`, and running an archive takes `-f`, `-q`, and `-t`. Because it
embeds the stub, the archive is larger than a plain `.sq` by the size of the
`squish` binary — worth it for handoff, not for routine storage.

When stderr is a terminal, a live status line (percent, bytes, throughput)
is shown while working, followed by a one-line summary. Pass `-q` /
`--quiet` to suppress both; errors are still reported.

## Library

C (see [examples/example.c](examples/example.c), full reference in
[docs/API.md](docs/API.md)):

```c
#include <squish.h>

void  *c, *d;  size_t cn, dn;
squish_compress_alloc(data, n, &c, &cn);
squish_decompress_alloc(c, cn, &d, &dn);   /* integrity-checked */
squish_free(c);  squish_free(d);
```

Multi-threaded variants take a thread count (0 = all cores) and, for
compression, a chunk size (0 = 16 MiB default):

```c
squish_compress_alloc_mt(data, n, &c, &cn, 0, 0, NULL, NULL);
squish_decompress_alloc_mt(c, cn, &d, &dn, 0, NULL, NULL);
```

Link with `-lsquish -lm -pthread` (or `pkg-config --cflags --libs squish`
after `make install`).

Python needs no wrapper — `libsquish.so` loads directly with ctypes; see
[examples/example.py](examples/example.py).

## Layout

```
squish.h            public API (documented header)
squish.c            library implementation
squish_cli.c        command-line tool
Makefile            .so / .a / CLI / DLL / tests / install
tests/              test suite (make test)
examples/           C and Python usage
docs/API.md         API reference
docs/FORMAT.md      byte-level format specification
SQUISH.md           algorithm design document
bench/              benchmark scripts, baselines, RESULTS.md
```

## Guarantees and limits

- Round-trip fidelity: every stream carries a 32-bit checksum of the
  original data; decompression fails loudly on corruption.
- `squish_compress_bound(n) = n + 17`: incompressible data falls back to
  stored mode, so output never exceeds input by more than the header.
- Inputs up to 4 GiB per stream.
- No global mutable state: concurrent (de)compressions on different buffers
  from multiple threads are safe.
- The model is the format: the constants in `squish.c` define the bitstream.
  Any change to them is a format break and needs a new magic number.
