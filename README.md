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
| zip -9 | 104,800,190 | 0.333 |
| bzip2 -9 | 84,058,237 | 0.267 |
| rar -m5 | 80,560,956 | 0.256 |
| xz -9e | 73,780,732 | 0.234 |
| **SQUISH** | **67,432,463** | **0.214** |

The trade: ~0.6 MB/s, symmetric (decompression costs the same as
compression), and ~150 MB of model memory per active block. SQUISH spends CPU
that zip/bzip2/rar don't and buys ratio with it.

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
./squish c bigfile bigfile.sqsh        # compress a file
./squish d bigfile.sqsh restored       # restore it (checksum-verified)
./squish c project project.sqsh        # pack a whole directory tree
./squish l project.sqsh                # list the archive's contents
./squish x project.sqsh src/main.c     # extract one file (or a subtree)
./squish d project.sqsh restored-dir   # recreate the whole tree under restored-dir
./squish -t 0 c bigfile bigfile.sqsh   # compress on all cores
./squish -t 0 -b 4 c big big.sqsh      # ... with 4 MiB blocks (more parallel,
                                       #     slightly worse ratio)
```

**Everything squish writes is one format: a SQUISH archive.** `input` may be a
file or a **directory** — either way `output` is an archive, with every file
compressed as its own member and directories, empty directories, and permission
bits preserved. That lets `l` list the contents and `x` pull out a single file
or subtree by seeking to just its blocks, without inflating the rest; `d`
restores a single file, or recreates a whole tree under `output`. Extraction
refuses absolute paths and `..`, so an archive never writes outside its target
directory, and members are stored sorted, so the output depends only on the
tree. The tradeoff for random access: each member's model starts cold, so many
tiny files compress a little worse than one solid stream. Library consumers get
the same operations through the `squish_archive_*` API; see
[docs/API.md](docs/API.md) and [docs/FORMAT.md](docs/FORMAT.md).

`-t N` compresses with N threads (`0` = all cores) by splitting each member
into independently modeled blocks — near-linear speedup, at a small ratio
cost because each block's model starts cold (about 1–2% at the default
16 MiB blocks, more at smaller `-b` sizes). The default `-t 1` keeps the
ratio-optimal single-block layout used for the results table above.
Decompression uses all cores the archive allows (`-t` caps it). Budget
~150 MB of model memory per thread.

When stderr is a terminal, a live status line (percent, bytes, throughput)
is shown while working, followed by a one-line summary. Pass `-q` /
`--quiet` to suppress both; errors are still reported.

## Library

C (see [examples/example.c](examples/example.c), full reference in
[docs/API.md](docs/API.md)):

```c
#include <squish.h>

/* Compress a buffer into a one-member archive and back. The trailing args are
 * threads (0 = all cores, 1 = serial), chunk size (0 = 16 MiB), and an
 * optional progress callback + user pointer. */
void  *c, *d;  size_t cn, dn;
squish_compress_alloc(data, n, &c, &cn, 1, 0, NULL, NULL);
squish_decompress_alloc(c, cn, &d, &dn, 0, NULL, NULL);   /* integrity-checked */
squish_free(c);  squish_free(d);
```

Pack and inspect a directory tree through the `squish_archive_*` API, reaching
any one member without inflating the rest:

```c
squish_archive_create("project", "project.sqsh", 0, 0, NULL, NULL);
squish_archive *a;
squish_archive_open("project.sqsh", &a);
squish_archive_extract_path(a, "src/main.c", &d, &dn);    /* just this file */
squish_free(d);  squish_archive_close(a);
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

- One on-disk format: every output is a SQUISH archive (magic `SQSH`).
- Round-trip fidelity: every coded block carries a 32-bit checksum of its
  original data; decompression fails loudly on corruption.
- Bounded expansion: incompressible data falls back to stored blocks, so
  `squish_compress_bound(n)` is guaranteed to hold the result.
- Up to 4 GiB per member.
- No global mutable state: concurrent (de)compressions on different buffers
  from multiple threads are safe.
- The model is the format: the constants in `squish.c` define the bitstream.
  Any change to them is a format break and needs a new version.
