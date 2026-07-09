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
./squish c bigfile bigfile.sq     # compress
./squish d bigfile.sq restored    # decompress (checksum-verified)
```

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

Link with `-lsquish -lm` (or `pkg-config --cflags --libs squish` after
`make install`).

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
