# SQUISH stream formats "SQ02" and "SQ01"

This document specifies the byte format completely enough to write an
independent decoder. Because SQUISH is a context-mixing design, the
container is trivial and the real specification is the **predictor**: the
decoder must reproduce the encoder's probability for every bit exactly,
which means every constant and update rule below is normative.

There are two containers: `SQ02`, a single model stream, and `SQ01`
(§1b), a multi-block wrapper holding independent `SQ02` streams so that
chunks can be coded in parallel.

## 1. Container

```
offset  size  field
0       4     magic: 'S' 'Q' '0' '2'
4       8     original size n, unsigned 64-bit little-endian
12      1     mode: 0 = context-mixed arithmetic stream, 1 = stored
13      ...   payload
last    4     checksum: FNV-1a 64 of the n original bytes, low 32 bits, LE
```

- Inputs are limited to n < 2^32 − 16.
- `mode 1` (stored): payload is the n original bytes verbatim; total file
  size is exactly n + 17. Encoders emit stored mode whenever the arithmetic
  stream would be at least as large (guaranteeing the n + 17 bound).
- `mode 0`: payload is the arithmetic-coded bitstream described below.
- FNV-1a 64: `h = 0xcbf29ce484222325; for each byte b: h ^= b; h *= 0x100000001b3`.

## 1b. Multi-block container "SQ01"

Produced by the multi-threaded encoder for inputs larger than one chunk.
The payload is a sequence of complete, independent `SQ02` streams; models
never carry state across chunk boundaries, which is what makes parallel
encode and decode possible.

```
offset       size  field
0            4     magic: 'S' 'Q' '0' '1'
4            8     total original size n, unsigned 64-bit little-endian
12           1     mode: 2 = multi-block
13           4     chunk count k >= 1, unsigned 32-bit little-endian
17           4*k   compressed size of each chunk, u32 LE each
17 + 4*k     ...   k chunks, each a complete SQ02 stream (§1)
```

- Chunk i decodes to bytes `[sum of previous chunks' sizes ...)` of the
  output; the original size of each chunk comes from its own SQ02 header.
- The compressed sizes must tile the rest of the file exactly, and the
  per-chunk original sizes must sum to n.
- Chunks must be `SQ02` streams: nested `SQ01` containers are invalid.
- There is no container-level checksum; each chunk carries its own (§1).
- Encoders chunk the input at a fixed chunk size (encoder choice; the
  reference default is 16 MiB, minimum 64 KiB), so the emitted stream
  depends only on the input and the chunk size — never on the thread
  count. An encoder that finds the multi-block stream no smaller than
  stored mode emits a single stored-mode `SQ02` stream instead, which
  preserves the n + 17 output bound.

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

`SQ02` (single stream) and `SQ01` (multi-block, §1b) are the current
versions. Any change to a constant, table size, initialization value, or
update rule in §§2–9 produces incompatible streams and requires a new
magic. (The magic `SQ01` previously named a pre-release single-stream
format without the mode byte and checksum; that format was never released
and no reader accepted it, so the magic has been reassigned to the
multi-block container, which is distinguishable by its mode byte 2.)
