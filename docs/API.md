# libsquish API reference

Everything lives in `squish.h`; link with `-lsquish -lm`. All functions are
exported with default visibility from `libsquish.so` (or `__declspec` on
Windows — define `SQUISH_DLL` when consuming the DLL, nothing when linking
the static library).

## Model of operation

libsquish is a **one-shot, whole-buffer** codec. The algorithm's match model
addresses the entire history, so streaming chunks independently would cost
ratio; hand the library complete buffers (or use the file helpers). Memory
per call is ~150 MB of model state plus your buffers, released before the
call returns.

**Thread safety:** the library has no global mutable state. Any number of
threads may compress/decompress concurrently as long as they don't share
buffers. A single call is single-threaded and CPU-bound (~0.5–0.7 MB/s).

**Status codes:** every function returns `int`; `SQUISH_OK` (0) on success,
negative `squish_status` otherwise. `squish_strerror` renders any code.

| code | meaning |
|---|---|
| `SQUISH_OK` (0) | success |
| `SQUISH_E_PARAM` (-1) | NULL pointer or invalid argument |
| `SQUISH_E_NOMEM` (-2) | allocation failure |
| `SQUISH_E_FORMAT` (-3) | bad magic, bad mode byte, size field, or truncation |
| `SQUISH_E_DSTSIZE` (-4) | destination capacity insufficient |
| `SQUISH_E_TOOBIG` (-5) | input ≥ `SQUISH_MAX_INPUT` (4 GiB − 16) |
| `SQUISH_E_IO` (-6) | file open/read/write failed (file helpers only) |
| `SQUISH_E_CHECKSUM` (-7) | payload decoded but integrity check failed |

---

## Introspection

```c
const char *squish_version(void);
```
Returns the library version string (`"1.0.0"`). Compare against
`SQUISH_VERSION_MAJOR/MINOR/PATCH` from the header you compiled with.

```c
const char *squish_strerror(int code);
```
Human-readable description of any status code. Static storage; never NULL.

## Sizing

```c
size_t squish_compress_bound(size_t src_len);
```
Worst-case compressed size: `src_len + 17` (13-byte header + 4-byte
checksum). This is a hard guarantee — when the coded stream would exceed the
stored size, compression falls back to stored mode — so a destination of
this capacity can never yield `SQUISH_E_DSTSIZE`.

```c
int squish_decompressed_size(const void *src, size_t src_len,
                             uint64_t *out_size);
```
Reads the original size from a compressed header. Needs only the first
12 bytes; does not validate the payload. Use it to size destination buffers.

## One-shot, caller-provided buffers

```c
int squish_compress(const void *src, size_t src_len,
                    void *dst, size_t *dst_len);
```
Compresses `src[0..src_len)` into `dst`. On entry `*dst_len` is the capacity
of `dst`; on success it becomes the number of bytes written. `src` is only
read. `src` and `dst` must not overlap. Fails with `SQUISH_E_DSTSIZE` if
capacity is insufficient (never happens at `squish_compress_bound(src_len)`
capacity), `SQUISH_E_TOOBIG` beyond 4 GiB, `SQUISH_E_NOMEM` if the ~150 MB
of model state can't be allocated.

```c
int squish_decompress(const void *src, size_t src_len,
                      void *dst, size_t *dst_len);
```
Decompresses and **verifies the checksum**. On entry `*dst_len` is the
capacity of `dst`; on success it becomes the decompressed size. If capacity
is too small, returns `SQUISH_E_DSTSIZE` and sets `*dst_len` to the required
size without touching `dst`. `SQUISH_E_CHECKSUM` means the container parsed
but the reconstructed data does not match what was compressed — treat the
output as garbage. Corrupt or truncated input never crashes; it produces an
error code.

## One-shot, library-allocated output

```c
int squish_compress_alloc(const void *src, size_t src_len,
                          void **dst, size_t *dst_len);
int squish_decompress_alloc(const void *src, size_t src_len,
                            void **dst, size_t *dst_len);
void squish_free(void *p);
```
Same semantics, but the library allocates `*dst` (trimmed to the exact
result size). On any error `*dst` is NULL. Release with `squish_free` — not
plain `free` — so the allocator always matches across DLL boundaries on
Windows.

## File helpers

```c
int squish_compress_file(const char *src_path, const char *dst_path);
int squish_decompress_file(const char *src_path, const char *dst_path);
```
Whole-file convenience wrappers (read fully into memory, process, write;
`dst_path` is overwritten). All buffer-level errors pass through;
`SQUISH_E_IO` covers filesystem trouble.

```c
typedef void (*squish_progress_fn)(uint64_t processed, uint64_t total,
                                   void *user);

int squish_compress_file2(const char *src_path, const char *dst_path,
                          squish_progress_fn progress, void *user);
int squish_decompress_file2(const char *src_path, const char *dst_path,
                            squish_progress_fn progress, void *user);
```
As above, plus progress reporting: `progress` (if non-NULL) is invoked with
the count of original — uncompressed — bytes processed so far, every 64 KiB
of progress and once with `processed == total` on completion. It is called
from the coding loop, so keep it cheap and do not call back into the
library. `user` is passed through untouched. With `progress == NULL` these
are identical to the plain versions.

## Multi-threaded variants

```c
int squish_threads(void);
```
Number of processors online (>= 1); what `nthreads = 0` selects below.

```c
int squish_compress_mt(const void *src, size_t src_len,
                       void *dst, size_t *dst_len,
                       int nthreads, size_t chunk_size,
                       squish_progress_fn progress, void *user);
int squish_decompress_mt(const void *src, size_t src_len,
                         void *dst, size_t *dst_len, int nthreads,
                         squish_progress_fn progress, void *user);

int squish_compress_alloc_mt(const void *src, size_t src_len,
                             void **dst, size_t *dst_len,
                             int nthreads, size_t chunk_size,
                             squish_progress_fn progress, void *user);
int squish_decompress_alloc_mt(const void *src, size_t src_len,
                               void **dst, size_t *dst_len, int nthreads,
                               squish_progress_fn progress, void *user);

int squish_compress_file_mt(const char *src_path, const char *dst_path,
                            int nthreads, size_t chunk_size,
                            squish_progress_fn progress, void *user);
int squish_decompress_file_mt(const char *src_path, const char *dst_path,
                              int nthreads,
                              squish_progress_fn progress, void *user);
```

Parallel counterparts of the functions above, with the same buffer,
allocation, file, and progress contracts. Compression splits the input
into `chunk_size`-byte chunks (`0` = `SQUISH_DEFAULT_CHUNK`, 16 MiB;
minimum `SQUISH_MIN_CHUNK`, 64 KiB) and codes each independently on a pool
of `nthreads` workers (`0` = all cores), emitting a multi-block `SQ01`
stream — see [FORMAT.md](FORMAT.md) §1b. Points worth knowing:

- **Output is deterministic**: it depends on the input and `chunk_size`
  only, never on `nthreads`.
- **Ratio cost**: each chunk's model starts cold, costing roughly 1–2% at
  the 16 MiB default (more at smaller chunks). Inputs no larger than one
  chunk produce a plain `SQ02` stream, bit-identical to `squish_compress`.
- **Bound preserved**: `squish_compress_bound` still holds — inputs that
  don't benefit fall back to a single stored-mode `SQ02` stream.
- The decompression functions read both formats; parallelism applies to
  `SQ01` streams (`SQ02` has a single sequential model). The plain
  (non-`_mt`) decompression functions also accept `SQ01`, serially.
- **Memory**: ~150 MB of model state per active worker.
- `progress` calls are serialized by the library but may arrive on worker
  threads; `processed` stays monotonic across the whole input.

---

## Linking recipes

```sh
# against the installed shared library
cc app.c $(pkg-config --cflags --libs squish)

# against the in-tree static library
cc app.c -I/path/to/squish /path/to/squish/libsquish.a -lm

# Windows, consuming squish.dll built via `make dll` (import lib libsquish.dll.a)
x86_64-w64-mingw32-gcc app.c -DSQUISH_DLL -lsquish -L/path/to/import/lib
```

From Python, load `libsquish.so` with `ctypes` directly —
`examples/example.py` is a complete, copyable wrapper (~40 lines).

## ABI and format stability

- The shared library uses semantic versioning; soname `libsquish.so.1`.
  Functions are added, never changed, within a major version.
- The bitstream format is identified by the magic (`SQ02` single stream,
  `SQ01` multi-block). The model constants in `squish.c` **are** the
  format: streams are only decodable by a build with identical constants.
  Readers reject other magics with `SQUISH_E_FORMAT`.
