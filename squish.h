/* Copyright (C) 2026  Paige Julianne Sullivan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* ============================================================================
 * squish.h — public API of libsquish, a context-mixing compressor
 *
 * SQUISH compresses by prediction: ten statistical models (byte contexts of
 * order 0-6, a word model, an auto-detected record model, and a long-range
 * match model) each estimate the probability of the next bit; an online-
 * trained logistic mixer fuses them and an arithmetic coder emits the result.
 * The decompressor runs the identical models in lockstep.
 *
 * One format
 *   Everything SQUISH produces is a *SQUISH archive* (magic "SQSH", see
 *   docs/FORMAT.md): a header, one independently-compressed member per file,
 *   and a compact seekable index. A lone buffer or file is just a one-member
 *   archive. There is no separate "stream" container — the archive is the
 *   only on-disk format. Internally a member's data is a list of coded
 *   *blocks* (each up to one chunk), which is what lets big members compress
 *   and decompress in parallel and lets a reader seek to any member.
 *
 * Characteristics
 *   ratio  : beats zip/bzip2/rar on 23/24 standard corpus files, xz -9e on 19/24
 *   speed  : ~0.5-0.7 MB/s, symmetric (compression == decompression cost)
 *   memory : ~150 MB of model state per active (de)compression, plus buffers
 *   limits : any single member up to 4 GiB - 16 bytes
 *
 * Thread safety
 *   The library keeps NO global mutable state. Every call allocates its own
 *   model state, so concurrent calls from different threads are safe as long
 *   as they do not share buffers. An archive reader handle is not itself
 *   thread-safe: serialize calls on one handle, or open one per thread.
 *
 * Parallelism
 *   Pass nthreads > 1 (or 0 = all cores) to split a member into chunk_size
 *   blocks and (de)compress them across a worker pool: near-linear speedup at
 *   a small ratio cost (each block's model starts cold). The bytes produced
 *   depend only on chunk_size, never on the thread count. Budget ~150 MB of
 *   model memory per thread.
 *
 * Integrity
 *   Every coded block carries a 32-bit checksum (FNV-1a 64, folded) of its
 *   original bytes; decompression verifies it and fails on mismatch.
 *
 * Minimal example
 *   size_t   cn;  void *c;
 *   squish_compress_alloc(data, n, &c, &cn, 0, 0, NULL, NULL);   // -> archive
 *   size_t   dn;  void *d;
 *   squish_decompress_alloc(c, cn, &d, &dn, 0, NULL, NULL);      // -> data
 *   squish_free(c); squish_free(d);
 * ==========================================================================*/
#ifndef SQUISH_H
#define SQUISH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- linkage ------------------------------------------------------------ */
#if defined(_WIN32)
#  if defined(SQUISH_BUILD_DLL)
#    define SQUISH_API __declspec(dllexport)
#  elif defined(SQUISH_DLL)
#    define SQUISH_API __declspec(dllimport)
#  else
#    define SQUISH_API
#  endif
#else
#  if defined(SQUISH_BUILD) && defined(__GNUC__)
#    define SQUISH_API __attribute__((visibility("default")))
#  else
#    define SQUISH_API
#  endif
#endif

/* --- version ------------------------------------------------------------ */
#define SQUISH_VERSION_MAJOR 1
#define SQUISH_VERSION_MINOR 0
#define SQUISH_VERSION_PATCH 0
#define SQUISH_VERSION_STRING "1.0.0"

/* --- status codes (all API functions return one of these) ---------------- */
typedef enum squish_status {
    SQUISH_OK          =  0,  /* success                                     */
    SQUISH_E_PARAM     = -1,  /* NULL pointer or invalid argument            */
    SQUISH_E_NOMEM     = -2,  /* memory allocation failed                    */
    SQUISH_E_FORMAT    = -3,  /* bad magic, bad header, or truncated data    */
    SQUISH_E_DSTSIZE   = -4,  /* dst buffer too small (see function docs)    */
    SQUISH_E_TOOBIG    = -5,  /* input larger than SQUISH_MAX_INPUT          */
    SQUISH_E_IO        = -6,  /* file open/read/write failed (file helpers)  */
    SQUISH_E_CHECKSUM  = -7   /* decompressed data failed integrity check    */
} squish_status;

/* Largest supported single member, in bytes. */
#define SQUISH_MAX_INPUT ((uint64_t)0xFFFFFFF0u)

/* Chunk sizing (the block size a member is split into for parallelism).
 * chunk_size = 0 selects the default; anything smaller is raised to the min. */
#define SQUISH_DEFAULT_CHUNK ((size_t)16 << 20)   /* 16 MiB */
#define SQUISH_MIN_CHUNK     ((size_t)64 << 10)   /* 64 KiB */

/* --- introspection ------------------------------------------------------ */

/* Library version, e.g. "1.0.0". Static string, never NULL. */
SQUISH_API const char *squish_version(void);

/* Human-readable description of a squish_status. Static string, never NULL.*/
SQUISH_API const char *squish_strerror(int code);

/* Number of processors online (>= 1). What nthreads = 0 selects below. */
SQUISH_API int squish_threads(void);

/* --- progress reporting -------------------------------------------------- */

/* `processed` counts bytes of ORIGINAL (uncompressed) data handled so far,
 * out of `total`; invoked periodically and once with processed == total on
 * completion. Called from the coding loop: keep it cheap, and do not call
 * back into the library from it. */
typedef void (*squish_progress_fn)(uint64_t processed, uint64_t total,
                                   void *user);

/* --- in-memory: compress a buffer to a one-member archive ---------------- */

/* Worst-case archive size for src_len input bytes. Guaranteed sufficient: if
 * dst has at least this capacity, squish_compress_alloc cannot fail for want
 * of room (incompressible data falls back to stored blocks). */
SQUISH_API size_t squish_compress_bound(size_t src_len);

/* Compress src[0..src_len) into a freshly allocated SQUISH archive holding a
 * single unnamed member. nthreads (0 = all cores, 1 = serial) and chunk_size
 * (0 = default) shape the member's blocks; the bytes depend only on
 * chunk_size. `progress` may be NULL. Free *dst with squish_free. On any
 * error *dst is NULL. Returns SQUISH_OK / E_PARAM / E_NOMEM / E_TOOBIG. */
SQUISH_API int squish_compress_alloc(const void *src, size_t src_len,
                                     void **dst, size_t *dst_len,
                                     int nthreads, size_t chunk_size,
                                     squish_progress_fn progress, void *user);

/* Decompress the single member of the archive src[0..src_len) into a freshly
 * allocated buffer (free with squish_free), verifying each block's checksum.
 * nthreads as above. Rejects an archive that does not hold exactly one file
 * member with SQUISH_E_FORMAT (use the squish_archive_* API for multi-member
 * archives). On any error *dst is NULL. Returns SQUISH_OK, E_FORMAT,
 * E_CHECKSUM, E_PARAM, E_NOMEM. */
SQUISH_API int squish_decompress_alloc(const void *src, size_t src_len,
                                       void **dst, size_t *dst_len,
                                       int nthreads,
                                       squish_progress_fn progress, void *user);

/* Read the total uncompressed size from an archive header (offset 24). Needs
 * only the first 32 bytes; does not decompress anything. Returns SQUISH_OK
 * and sets *out_size, or SQUISH_E_FORMAT / SQUISH_E_PARAM. The size is a
 * claim by the producer: when sizing an allocation from untrusted input,
 * impose your own limit as well. */
SQUISH_API int squish_content_size(const void *src, size_t src_len,
                                   uint64_t *out_size);

/* Free a buffer returned by an *_alloc / *_extract function. NULL is a no-op.*/
SQUISH_API void squish_free(void *p);

/* --- whole-file convenience helpers -------------------------------------- */

/* Compress the single file src_path into an archive at dst_path (a one-member
 * archive; the member is named with src_path's basename). nthreads / chunk_size
 * / progress as squish_compress_alloc. SQUISH_E_IO covers file trouble. */
SQUISH_API int squish_compress_file(const char *src_path, const char *dst_path,
                                    int nthreads, size_t chunk_size,
                                    squish_progress_fn progress, void *user);

/* Decompress a one-member archive at src_path to the file dst_path. Rejects a
 * multi-member archive with SQUISH_E_FORMAT (extract those with the archive
 * API). nthreads / progress as squish_decompress_alloc. */
SQUISH_API int squish_decompress_file(const char *src_path, const char *dst_path,
                                      int nthreads,
                                      squish_progress_fn progress, void *user);

/* --- seekable archives --------------------------------------------------- *
 *
 * The archive is the SQUISH format (docs/FORMAT.md): a header, one
 * independently-compressed member per file, and a compact index (paths +
 * per-file block layout) so a reader can view the header, list members, and
 * inflate a single file or subtree by seeking straight to it — without ever
 * touching the rest of the archive.
 */

/* Opaque archive reader. Not thread-safe: serialize calls on one handle, or
 * open one handle per thread. Different handles are independent. */
typedef struct squish_archive squish_archive;

/* Archive-wide header, filled by squish_archive_info_get. */
typedef struct squish_archive_info {
    uint32_t version;       /* container version (currently 1)               */
    uint32_t flags;         /* reserved, currently 0                         */
    uint64_t entry_count;   /* number of members (files + directories)       */
    uint64_t total_size;    /* sum of member uncompressed sizes              */
    uint64_t chunk_size;    /* block size members were split at              */
} squish_archive_info;

/* One member, filled by squish_archive_stat. `path` points into the handle
 * and stays valid until squish_archive_close. For the unnamed member of a
 * buffer/blob archive, `path` is the empty string "". */
typedef struct squish_archive_entry {
    const char *path;        /* relative, '/'-separated, UTF-8, NUL-terminated */
    uint64_t    size;        /* uncompressed size (0 for a directory)         */
    uint64_t    stored_size; /* total compressed size of the member's blocks  */
    uint32_t    mode;        /* unix permission bits (low 9), informational   */
    int         is_dir;      /* 1 = directory, 0 = regular file               */
} squish_archive_entry;

/* Nonzero iff the first bytes are a valid archive header (magic + version).
 * A cheap sniff for telling an archive from other data; needs 12 bytes. */
SQUISH_API int squish_archive_probe(const void *data, size_t len);

/* Open an archive for reading. Reads and validates only the header and the
 * (compressed) index — work proportional to the number of members, not the
 * archive size. On success *out owns resources freed by squish_archive_close;
 * on any error *out is NULL. Returns SQUISH_OK, SQUISH_E_IO (file trouble),
 * SQUISH_E_FORMAT (not an archive / corrupt), SQUISH_E_NOMEM, SQUISH_E_PARAM.
 * squish_archive_open keeps the file open and seeks per extraction;
 * squish_archive_open_memory borrows `data` (which must outlive the handle). */
SQUISH_API int  squish_archive_open(const char *path, squish_archive **out);
SQUISH_API int  squish_archive_open_memory(const void *data, size_t len,
                                           squish_archive **out);
SQUISH_API void squish_archive_close(squish_archive *a);

/* Header and listing (all served from the in-memory index; no decompression).
 * stat's `index` is 0..count-1; find returns SQUISH_E_FORMAT if `path` is not
 * a member. */
SQUISH_API int      squish_archive_info_get(const squish_archive *a,
                                            squish_archive_info *out);
SQUISH_API uint64_t squish_archive_count(const squish_archive *a);
SQUISH_API int      squish_archive_stat(const squish_archive *a, uint64_t index,
                                        squish_archive_entry *out);
SQUISH_API int      squish_archive_find(const squish_archive *a,
                                        const char *path, uint64_t *index_out);

/* Extract ONE member into a library-allocated buffer (free with squish_free);
 * reads and inflates only that member's blocks and verifies their checksums. A
 * directory member yields SQUISH_E_PARAM. On any error *out is NULL. The
 * _path form is squish_archive_find + squish_archive_extract. */
SQUISH_API int squish_archive_extract(squish_archive *a, uint64_t index,
                                      void **out, size_t *out_len);
SQUISH_API int squish_archive_extract_path(squish_archive *a, const char *path,
                                           void **out, size_t *out_len);

/* Extract to the filesystem, inflating only what is needed. _to_file writes a
 * single file member to dst_path (parent directories must already exist).
 * _extract_subtree recreates, under dst_root, every member whose path is
 * `prefix` or lies beneath it; prefix NULL or "" extracts the whole archive.
 * `cb` (may be NULL) reports uncompressed bytes written so far. */
SQUISH_API int squish_archive_extract_to_file(squish_archive *a,
                                              const char *path,
                                              const char *dst_path);
SQUISH_API int squish_archive_extract_subtree(squish_archive *a,
                                              const char *prefix,
                                              const char *dst_root,
                                              squish_progress_fn cb, void *user);

/* Build a seekable archive at archive_path from the directory tree dir_path,
 * compressing each file as its own member (nthreads / chunk_size as above).
 * `cb` (may be NULL) reports total uncompressed bytes packed so far. Returns
 * SQUISH_OK or SQUISH_E_IO / E_NOMEM / E_PARAM / E_TOOBIG. */
SQUISH_API int squish_archive_create(const char *dir_path,
                                     const char *archive_path,
                                     int nthreads, size_t chunk_size,
                                     squish_progress_fn cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* SQUISH_H */
