# SQUISH — a context-mixing file compressor

SQUISH is a single-file C compressor built around one idea: **compression is
prediction**. If you can estimate the probability of the next bit accurately,
an arithmetic coder converts those probabilities into a near-optimal code for
free (Shannon). So all of the design effort goes into one place: a fast
online-learning predictor that adapts to whatever kind of data it is reading.

zip (DEFLATE), bzip2 (BWT), and RAR each commit to one structural model of
data. SQUISH instead runs **ten specialist models simultaneously** and learns,
bit by bit, which ones to trust for the current file — so it behaves like an
LZ compressor on repetitive data, like a high-order PPM compressor on text,
and like a table/raster codec on records, without being told which is which.

## The pipeline (per bit)

```
                 ┌─ order-0..order-6 byte contexts (7 models)
   input bit ────┤  word model (current alphanumeric word hash)
   history  ────┤  record model (auto-detected row length R: cell above, column)
                 └─ match model (6-byte rolling hash -> longest recent repeat)
                          │ each model outputs P(bit=1) from an adaptive counter
                          ▼
              logistic mixer  — tiny 1-layer net, 11 inputs, 1024 weight sets
              (selected by prev byte + match/record state), trained online by
              gradient descent in the log-odds ("stretch") domain
                          ▼
              2 chained APM/SSE stages — recalibrate the mixed probability
              against what actually happened in similar contexts
                          ▼
              carryless binary arithmetic coder (32-bit)
```

Decompression runs the *identical* predictor in lockstep — the decoder makes
the same predictions, so it only needs the arithmetic-coded corrections.
There is no dictionary, no block structure, no Huffman tables in the format:
**the model is the format.**

## The pieces

- **Adaptive counters.** Every context maps (by 64-bit hash into a 4M-slot
  table) to a 32-bit cell: 16-bit probability + 16-bit confidence count.
  Update rule `p += (bit - p) / (n+2)` starts plastic and hardens as evidence
  accumulates — fast convergence on fresh contexts, stability on old ones.

- **Match model.** A 4M-entry hash of the last 6 bytes finds the most recent
  identical context; the model then predicts that history simply repeats,
  with confidence learned per match-length bucket. This gives SQUISH the
  long-range repeat power of LZ77 with an *unbounded* window (the whole file),
  but probabilistically — a broken match costs a fraction of a bit, not a
  malformed token.

- **Record model.** A vote table tracks recurring distances between equal
  bytes; when one distance dominates (spreadsheet rows, raster scan lines,
  fixed-size DB records), two extra contexts light up: the byte one record
  above, and (column, byte-above, byte-two-above). This is why SQUISH beats
  even RAR's specialized filters on `kennedy.xls`.

- **Logistic mixing.** Model opinions are combined as weighted sums of
  log-odds, with the weight vector chosen by context and trained by online
  gradient descent on actual coding loss. Useless models are learned away in
  a few KB; on a text file the word model earns a large weight, on a binary
  the record model does.

- **APM/SSE refinement.** Two small secondary tables map (context, mixed
  probability) -> corrected probability, fixing systematic miscalibration.

## Format

There is one on-disk format, the **SQUISH archive** (magic `"SQSH"`): a header,
one compressed member per file, and a compressed index of paths and per-file
block layout. A member's bytes are one or more **coded blocks**, each a mode
byte (coded/stored) · payload · 32-bit checksum of the block's original data.
Incompressible data falls back to stored blocks, so output stays close to
input; decompression verifies every block's checksum. A lone file or buffer is
just a one-member archive. Full byte-level spec: [docs/FORMAT.md](docs/FORMAT.md).

The model pipeline is strictly sequential within a block — every bit's
prediction depends on the previous bit's update — so multi-core operation comes
from cutting a member into fixed-size blocks: independent blocks compress and
decompress in parallel (`squish -t`), trading ~1–2% of ratio for near-linear
speedup. Because the archive index records each member's block layout, a reader
can seek straight to any member, or any block of it, without touching the rest.

## Usage

```
make                              # libsquish.so + squish CLI
./squish c input output.sqsh      # compress a file (or a directory tree)
./squish d output.sqsh restored   # decompress (checksum-verified)
./squish l archive.sqsh           # list an archive's contents
./squish x archive.sqsh a/b.txt   # extract one file (or subtree) from an archive
```

`input` may be a file or a directory; either way `output` is a SQUISH archive,
each file its own member, so `l` lists the contents and `x` pulls out a single
file or subtree without inflating the rest; `d` restores a single file or
recreates a whole tree under `restored`. The same operations are available to
library consumers through the `squish_archive_*` API. See
[docs/FORMAT.md](docs/FORMAT.md) and [docs/API.md](docs/API.md).

SQUISH is also a library — `squish.h` + `libsquish.so` (or `make dll` for
Windows); see [docs/API.md](docs/API.md) and `examples/`.

Memory: ~150 MB of model state per active block + buffers. Speed: ~0.5–0.7 MB/s,
symmetric (decompression runs the same models). That is the honest trade:
SQUISH spends CPU that zip/bzip2/rar don't, and buys ratio with it.

## Lineage

SQUISH composes known primitives — context mixing is the architecture behind
the PAQ family (Mahoney et al.), which holds most compression-ratio records.
The specific model set, the auto-detected record contexts, the counter and
mixer parameterization, and the implementation are original to this project.
