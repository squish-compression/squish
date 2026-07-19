# The SQUISH archive format

SQUISH has one on-disk format: the **archive**. A lone file or buffer is a
one-member archive; a directory is a many-member archive. This document
specifies the bytes completely enough to write an independent decoder.
Because SQUISH is a context-mixing design the container is trivial, and the
real specification is the **predictor** (§§2–9): the decoder must reproduce
the encoder's probability for every bit exactly, so every constant and update
rule there is normative.

An archive is a fixed 64-byte header, then the member data region (each file's
contents as one or more coded blocks, concatenated), then a compressed index.
A reader inflates the small index once and can then reach any member — or any
block of a member — by seeking straight to it. All integers are little-endian.

## 1. Archive container

```
offset  size  field
0       8     magic: 'S' 'Q' 'S' 'H' 0x0D 0x0A 0x1A 0x0A
8       4     version u32 (currently 1)
12      4     flags u32: bit 0 = SINGLE (one file/blob, not a packed tree)
16      8     entry_count u64      (members: files + directories)
24      8     total_size u64       (sum of member uncompressed sizes)
32      8     chunk_size u64       (block size members were split at, >= 1)
40      8     index_offset u64     (absolute offset of the index block)
48      8     index_size u64       (index block length, bytes)
56      8     index_orig_size u64  (index length after decompression)
64      ...   member data region, then the index block at index_offset
```

The index is a single coded block (§1a) whose decoded bytes are `entry_count`
entries in pre-order (directories before their contents, siblings sorted by
name — so the archive depends only on the tree, not on directory iteration
order). Each entry:

```
offset  size  field
0       1     type: 0 = regular file, 1 = directory
1       4     mode u32: unix permission bits (low 9); informational,
              synthesized as 0644/0755 by producers without them
5       8     orig_size u64   (uncompressed size; 0 for a directory)
13      8     data_off u64    (offset of the member's first block; 0 for a
                               directory or an empty file)
21      4     path_len P u32
25      ...   B block sizes, u64 each, where B = ceil(orig_size / chunk_size):
              the compressed length of each block (none for a dir/empty file)
...     P     path: relative, '/'-separated, UTF-8, no terminator. P may be 0
              only for the lone unnamed member of a SINGLE blob archive.
```

A member's `B` blocks are stored contiguously from `data_off`; block *i*
decompresses to `min(chunk_size, orig_size − i·chunk_size)` bytes, and its
compressed length is the *i*-th size in the entry. The block count and every
block's original size are therefore derived from `orig_size` and `chunk_size`
— only the compressed sizes are stored.

To read one member: decode the index block once, look the path up, then decode
its blocks (in parallel if you like) straight from `data_off`. Each block
verifies its own checksum (§1a), so there is no archive-level checksum.

A reader must reject (as a format error): a wrong magic or version; a
`chunk_size` of 0; an index block that does not lie wholly after the header and
inside the file; entries that do not tile `index_orig_size` exactly; for a
file, a block region not contained in `[64, index_offset)` or any block size
below the 5-byte minimum; for a directory, any nonzero `orig_size`/`data_off`;
and — so a member can never be written outside the extraction root — any path
that is absolute or contains an empty, `.`, or `..` component or a `\` or `:`
character.

The `SINGLE` flag (bit 0 of `flags`) marks an archive that holds exactly one
file member and is meant to be restored as a single file rather than unpacked
into a directory; it is set when compressing one file or buffer and clear when
packing a directory tree.

## 1a. Coded block

The atom of the format. A block holds up to `chunk_size` original bytes:

```
offset  size  field
0       1     mode: 0 = context-mixed arithmetic stream, 1 = stored
1       ...   payload
last    4     checksum: FNV-1a 64 of the block's original bytes, low 32 bits, LE
```

- The block's original and compressed sizes come from the enclosing index, so
  the block carries neither a magic nor a length.
- `mode 1` (stored): payload is the original bytes verbatim; the block is
  exactly `orig + 5` bytes. Encoders emit stored mode whenever the arithmetic
  stream would be at least as large, which bounds any block at `orig + 5`.
- `mode 0`: payload is the arithmetic-coded bitstream of §2.
- FNV-1a 64: `h = 0xcbf29ce484222325; for each byte b: h ^= b; h *= 0x100000001b3`.
- A member is limited to fewer than 2^32 − 16 bytes.

## 2. Arithmetic coder

Carryless binary range coder over 32-bit registers, coding bits MSB-first
within each byte, all n bytes in order.

Encoder state `(x1, x2)` starts `(0, 0xFFFFFFFF)`. To code bit `y` with
probability `p16` = P(y = 1) as a 16-bit integer:

```
range = x2 - x1
xmid  = x1 + (range >> 16) * p16 + (((range & 0xFFFF) * p16) >> 16)
y = 1 -> x2 = xmid        y = 0 -> x1 = xmid + 1
while (x1 ^ x2) & 0xFF000000 == 0:
    emit byte x2 >> 24;  x1 <<= 8;  x2 = (x2 << 8) | 0xFF
```

Flush after the last bit: emit the top byte of `x1` four times (shifting
`x1` left 8 each time). The decoder mirrors: it preloads 4 bytes into `x`,
computes the same `xmid`, decides `y = (x <= xmid)`, and pulls one byte into
`x` per renormalization. Reads past the end of the payload return 0xFF.

The coded probability is derived from the predictor's 12-bit output `pr` as
`p16 = (pr << 4) | 8` (so p16 ∈ [24, 65512]; the interval never collapses).

## 3. Probability domains

- Counters and APM cells store P(1) as 16 bits (0 ≈ 0, 65535 ≈ 1).
- The mixer works in the log-odds ("stretch") domain:
  `stretch(p) = ln(p / (1-p))`, fixed-point with 2048 ≈ 8.0, i.e. tables
  built from `squash(d) = 4096 / (1 + exp(-d / 256))` for d ∈ [-2048, 2047],
  and `stretch` its monotone inverse over 12-bit probabilities. Both tables
  are computed at startup exactly as in `squish.c` (`ctx_new`), including
  integer truncation — they are part of the format.

## 4. Adaptive counters

A counter is 32 bits: high 16 = probability p, low 16 = confidence n,
initialized to p = 32768, n = 0. After coding bit y:

```
p += ((y ? 65535 : 0) - p) * RATE[n] >> 16     where RATE[n] = 65536 / (n + 2)
n  = min(n + 1, 255)
```

## 5. The ten models

Byte history: `c8` = last 8 bytes (64-bit shift register), `b1` = previous
byte, `pos` = bytes completed, `buf[0..pos)` = data so far. Within a byte,
`c0` = partial byte with a leading-1 sentinel (starts at 1), `bitn` = bits
done. The 64→32 hash is
`hh(x, salt): x = (x + salt) * 0x9E3779B97F4A7C15; x ^= x>>29;
x *= 0xBF58476D1CE4E5B9; x ^= x>>32; return low 32 bits`.

Eight hashed models share the layout: a table of 2^22 counters, indexed per
bit by `(cxt ^ (c0 * 2654435761)) & (2^22 - 1)`. Their per-byte contexts:

| # | model | context (recomputed after every byte) |
|---|---|---|
| 0 | order-1 | `hh(c8 & 0xFF, 0x11)` |
| 1 | order-2 | `hh(c8 & 0xFFFF, 0x22)` |
| 2 | order-3 | `hh(c8 & 0xFFFFFF, 0x33)` |
| 3 | order-4 | `hh(c8 & 0xFFFFFFFF, 0x44)` |
| 4 | order-6 | `hh(c8 & 0xFFFFFFFFFFFF, 0x66)` |
| 5 | word | `hh(hword, 0x77)`, active only while `hword != 0` |
| 6 | record A | `hh((R << 8) \| buf[pos-R], 0x88)`, active if R ≥ 2 and pos ≥ 2R |
| 7 | record B | `hh(((pos % R) << 16) \| (buf[pos-R] << 8) \| buf[pos-2R], (R << 8) ^ 0x99)`, same condition |

Plus: an **order-0** model (256 counters indexed by `c0` directly) and the
**match model** (§7). Inactive models contribute stretch 0 and are not
updated.

**Word hash:** after each byte b that is a letter, digit, or ≥ 128:
`hword = (hword + (b | 32) + 1) * 0x9E3779B1`; any other byte resets
`hword = 0`.

## 6. Record-length detection

`lastpos[256]` tracks the last position of each byte value. After each byte
b: `d = pos - lastpos[b]`; if 2 ≤ d ≤ 2048, `rvote[d]++`; then
`lastpos[b] = pos`. Every 2048 bytes, find the maximum vote `vmax` at
distance `dmax`, then halve every vote. If `vmax ≥ 600` set `R = dmax`;
else if `vmax < 300` set `R = 0` (hysteresis in between).

## 7. Match model

`mt` is a table of 2^22 positions. After each byte, with
`mh = hh(c8 & 0xFFFFFFFFFFFF, 0xABCD) & (2^22 - 1)`:

- If a match is live: it survives if `buf[mptr] == b` (then `mptr++`,
  `mlen++` capped at 65535), else `mlen = 0`.
- If no match is live and `pos ≥ 6`: candidate `c = mt[mh]`; adopt
  (`mptr = c`, `mlen = 6`) if `c ≥ 3`, `c < pos`, and the last three bytes
  at both positions agree.
- Then `mt[mh] = pos` (only once `pos ≥ 6`).

Per bit, while the match is valid and its predicted byte still agrees with
`c0`: expected bit `e = (buf[mptr] >> (7 - bitn)) & 1`; the model's
probability comes from one of 64 counters indexed `min(mlen, 31) * 2 + e`
(initialized to 12000·2^16 for e = 0 and 53536·2^16 for e = 1). A bit that
contradicts `e` silences the model until the next byte boundary.

## 8. Mixer

11 inputs per bit, each a stretch value: constant 128 (bias), order-0, the
eight hashed models, the match model. Weight sets: 1024 vectors of 11
signed 32-bit weights, all initialized to 65536/11 = 5957; the active set is
`(match_live << 9) | ((R ≥ 2) << 8) | b1`.

```
dot  = Σ w[i] * st[i]                (64-bit)
pmix = squash(clamp(dot >> 16, ±2047))          — 12-bit
```

After the bit is known: `err = (y << 12) - pmix`, and for each input
`w[i] += (st[i] * err) >> 11`, clamped to ±2^24.

## 9. APM chain

Two SSE stages refine `pmix`. A stage is a table of 33-entry rows (one row
per context), row entries initialized to `squash((j - 16) * 128) << 4`,
j = 0..32. Lookup: `s = stretch(pr) + 2048`, `j = s >> 7`, `w = s & 127`,
output `(t[j] * (128 - w) + t[j+1] * w) >> 11`; the entry nearest the input
(`j + (w >> 6)`) is updated by `v += ((y ? 65535 : 0) - v) >> 7`.

```
pr = pmix
pr = (pr + 3 * APM1(pr | context c0))            >> 2      (256 rows)
pr = (pr + 3 * APM2(pr | context (b1 << 8)|c0))  >> 2      (65536 rows)
pr = clamp(pr, 1, 4094)
```

`pr` then feeds the coder (§2), and every component updates with the coded
bit — predict, code, update, in that order, for encoder and decoder alike.

## 10. Versioning

Version 1, magic `SQSH` followed by `\r\n\x1a\n`, is the current and only
format. Any change to a constant, table size, initialization value, or update
rule in §§2–9 produces incompatible blocks and requires a new version. The
pre-release stream formats `SQ02`/`SQ01` and the archive formats
`SQAR01`/`SQAR02` were never released and are not read by version-1 tools; the
magic distinguishes them.
