# libsquish API reference

Everything lives in `squish.h`; link with `-lsquish -lm`. All functions are
exported with default visibility from `libsquish.so` (or `__declspec` on
Windows — define `SQUISH_DLL` when consuming the DLL, nothing when linking
the static library).

## Model of operation

libsquish has **one on-disk format, the SQUISH archive**. A buffer or a single
file becomes a one-member archive; a directory tree becomes a many-member
archive. Inside, a member's bytes are one or more independently-coded *blocks*
(each up to `chunk_size`), which is what lets big members (de)compress in
parallel and lets a reader seek to any member. The full byte layout is in
[FORMAT.md](FORMAT.md).

The algorithm's match model addresses the whole of a block, so the ratio is
best when a member is one block; splitting into blocks trades roughly 1–2% of
ratio (at the 16 MiB default) for parallelism. Memory per active block is
~150 MB of model state plus your buffers, released as blocks finish.

**Thread safety:** the library has no global mutable state. Any number of
threads may compress/decompress concurrently as long as they don't share
buffers. Pass `nthreads` to parallelize *within* one call; an archive reader
handle, by contrast, is not thread-safe — use one per thread or serialize.

**Status codes:** every function returns `int`; `SQUISH_OK` (0) on success,
negative `squish_status` otherwise. `squish_strerror` renders any code.

| code | meaning |
|---|---|
| `SQUISH_OK` (0) | success |
| `SQUISH_E_PARAM` (-1) | NULL pointer or invalid argument |
| `SQUISH_E_NOMEM` (-2) | allocation failure |
| `SQUISH_E_FORMAT` (-3) | bad magic/header, corrupt archive, or truncation |
| `SQUISH_E_DSTSIZE` (-4) | destination capacity insufficient |
| `SQUISH_E_TOOBIG` (-5) | a member ≥ `SQUISH_MAX_INPUT` (4 GiB − 16) |
| `SQUISH_E_IO` (-6) | file open/read/write failed (file helpers only) |
| `SQUISH_E_CHECKSUM` (-7) | a block decoded but its integrity check failed |

---

## Introspection

```c
const char *squish_version(void);      /* "1.0.0" */
const char *squish_strerror(int code); /* human-readable, static, never NULL */
int         squish_threads(void);      /* processors online; what nthreads=0 picks */
```

## Progress reporting

```c
typedef void (*squish_progress_fn)(uint64_t processed, uint64_t total,
                                   void *user);
```
`progress` (wherever accepted; may be NULL) is invoked with the count of
original — uncompressed — bytes handled so far, periodically and once with
`processed == total` on completion. It is called from the coding loop, so keep
it cheap and do not call back into the library. Under multiple threads calls
are serialized by the library but may arrive on worker threads; `processed`
stays monotonic across the whole member.

## In-memory: buffer ⇄ one-member archive

```c
size_t squish_compress_bound(size_t src_len);
```
Worst-case archive size for `src_len` input bytes — a hard guarantee: a buffer
of at least this capacity can always hold the result, because incompressible
data falls back to stored blocks.

```c
int squish_compress_alloc(const void *src, size_t src_len,
                          void **dst, size_t *dst_len,
                          int nthreads, size_t chunk_size,
                          squish_progress_fn progress, void *user);
```
Compresses `src[0..src_len)` into a freshly allocated archive holding a single
unnamed member. `nthreads` (`0` = all cores, `1` = serial) and `chunk_size`
(`0` = `SQUISH_DEFAULT_CHUNK`, 16 MiB; raised to `SQUISH_MIN_CHUNK`, 64 KiB, if
smaller) shape the member's blocks. **The bytes produced depend only on
`chunk_size`, never on `nthreads`.** On any error `*dst` is NULL. Free with
`squish_free`.

```c
int squish_decompress_alloc(const void *src, size_t src_len,
                            void **dst, size_t *dst_len,
                            int nthreads, squish_progress_fn progress, void *user);
```
Restores the single member of the archive into a freshly allocated buffer,
verifying every block's checksum. Rejects an archive that does not hold exactly
one file member with `SQUISH_E_FORMAT` (use the `squish_archive_*` API for
multi-member archives). `SQUISH_E_CHECKSUM` means a block decoded but the data
does not match what was compressed — treat the output as garbage. Corrupt or
truncated input never crashes; it returns an error.

```c
int  squish_content_size(const void *src, size_t src_len, uint64_t *out_size);
void squish_free(void *p);
```
`content_size` reads the archive's total uncompressed size from the header
(needs 32 bytes; no decompression). Release any `*_alloc`/`*_extract` buffer
with `squish_free` — not plain `free` — so the allocator matches across DLL
boundaries on Windows.

## File helpers

```c
int squish_compress_file(const char *src_path, const char *dst_path,
                         int nthreads, size_t chunk_size,
                         squish_progress_fn progress, void *user);
int squish_decompress_file(const char *src_path, const char *dst_path,
                           int nthreads, squish_progress_fn progress, void *user);
```
`compress_file` packs one file into a one-member archive at `dst_path` (the
member takes the source's basename, and the archive's `SINGLE` flag is set).
`decompress_file` restores a one-member archive to `dst_path`, rejecting a
multi-member archive with `SQUISH_E_FORMAT` (extract those with the archive
API). `nthreads`/`chunk_size`/`progress` behave as above; `SQUISH_E_IO` covers
filesystem trouble.

---

## Archives

A directory tree is packed into a **seekable archive**: a header, one
independently-compressed member per file, and a compressed index of paths and
per-file block layout. A reader inflates the small index at open time, then
reaches any single member by seeking to its blocks — the rest of the archive is
never touched. The container is specified byte-for-byte in
[FORMAT.md](FORMAT.md) §1.

```c
int squish_archive_create(const char *dir_path, const char *archive_path,
                          int nthreads, size_t chunk_size,
                          squish_progress_fn cb, void *user);
```
Walks `dir_path` and writes a seekable archive to `archive_path`, compressing
each file as its own member. `nthreads`/`chunk_size` behave as above (a file
larger than one chunk is split into parallel blocks); `cb` reports total
uncompressed bytes packed. Directories (including empty ones) and unix mode
bits are recorded; sockets, fifos, and dangling links are skipped. Members are
stored pre-order, siblings sorted, so the archive depends only on the tree —
not on directory iteration order.

```c
int  squish_archive_probe(const void *data, size_t len);   /* needs 12 bytes */
int  squish_archive_open(const char *path, squish_archive **out);
int  squish_archive_open_memory(const void *data, size_t len, squish_archive **out);
void squish_archive_close(squish_archive *a);
```
`open` reads and validates **only the header and the index** — work
proportional to the member count, not the archive size — keeping the file open
to seek per extraction. `open_memory` borrows a buffer that must outlive the
handle (it must not be freed until `close`). `probe` is a cheap sniff for
telling an archive from other data. A handle is not thread-safe; use one per
thread, or serialize calls.

```c
int      squish_archive_info_get(const squish_archive *a, squish_archive_info *out);
uint64_t squish_archive_count(const squish_archive *a);
int      squish_archive_stat(const squish_archive *a, uint64_t index,
                             squish_archive_entry *out);
int      squish_archive_find(const squish_archive *a, const char *path,
                             uint64_t *index_out);
```
Header and listing, all served from the in-memory index with no decompression.
`info` exposes `version`, `flags` (bit 0 = `SINGLE`), `entry_count`,
`total_size`, and `chunk_size`. `stat` fills a `squish_archive_entry` (`path`,
`size`, `stored_size`, `mode`, `is_dir`); its `path` points into the handle and
is valid until `close` (it is `""` for the unnamed member of a blob archive).
`find` returns `SQUISH_E_FORMAT` when the path is not a member (a trailing `/`
on the query is ignored).

```c
int squish_archive_extract(squish_archive *a, uint64_t index,
                           void **out, size_t *out_len);
int squish_archive_extract_path(squish_archive *a, const char *path,
                                void **out, size_t *out_len);
```
Inflate **one** member into a library-allocated buffer (free with
`squish_free`), reading and decoding only that member's blocks and verifying
their checksums. A directory member returns `SQUISH_E_PARAM`.

```c
int squish_archive_extract_to_file(squish_archive *a, const char *path,
                                   const char *dst_path);
int squish_archive_extract_subtree(squish_archive *a, const char *prefix,
                                   const char *dst_root,
                                   squish_progress_fn cb, void *user);
```
Extract to the filesystem, inflating only what is needed. `_to_file` writes a
single file member to `dst_path` (its parent directories must already exist).
`_extract_subtree` recreates, under `dst_root`, every member whose path is
`prefix` or lies beneath it — each landing at `dst_root/<archive path>`;
`prefix` NULL or `""` extracts the whole archive. Member paths are re-validated
against traversal on the way out, so an archive can never write outside
`dst_root`.

```c
/* list every member, then pull one file out — reading only its blocks */
squish_archive *a;
if (squish_archive_open("photos.sqsh", &a) == SQUISH_OK) {
    uint64_t n = squish_archive_count(a);
    for (uint64_t i = 0; i < n; i++) {
        squish_archive_entry e;
        squish_archive_stat(a, i, &e);
        printf("%s%s (%llu bytes)\n", e.path, e.is_dir ? "/" : "",
               (unsigned long long)e.size);
    }
    void *buf; size_t len;
    if (squish_archive_extract_path(a, "2026/trip/cover.jpg", &buf, &len) == SQUISH_OK) {
        /* ... use buf[0..len) ... */
        squish_free(buf);
    }
    squish_archive_close(a);
}
```

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
`examples/example.py` is a complete, copyable wrapper.

## ABI and format stability

- The shared library uses semantic versioning; soname `libsquish.so.1`.
  Functions are added, never changed, within a major version.
- There is one on-disk format, identified by the magic `SQSH` and a version
  field. The model constants in `squish.c` **are** the format: archives are
  only decodable by a build with identical constants. Readers reject other
  magics or versions with `SQUISH_E_FORMAT`.
