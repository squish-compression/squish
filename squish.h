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
 * Characteristics
 *   ratio  : beats zip/bzip2/rar on 23/24 standard corpus files, xz -9e on 19/24
 *   speed  : ~0.5-0.7 MB/s, symmetric (compression == decompression cost)
 *   memory : ~150 MB of model state per active (de)compression, plus buffers
 *   limits : inputs up to 4 GiB - 16 bytes
 *
 * Thread safety
 *   The library keeps NO global mutable state. Every call allocates its own
 *   model state, so concurrent calls from different threads are safe as long
 *   as they do not share src/dst buffers.
 *
 * Parallelism
 *   The squish_*_mt functions split the input into independent chunks and
 *   run one model per chunk across worker threads ("SQ01" multi-block
 *   streams): near-linear speedup at a small ratio cost (each chunk's model
 *   starts cold). Output depends only on the chunk size, never on the
 *   thread count. Plain squish_decompress* reads both formats. Budget
 *   ~150 MB of model memory per thread.
 *
 * Integrity
 *   Every stream carries a 32-bit checksum (FNV-1a 64, folded) of the
 *   original data; squish_decompress verifies it and fails on mismatch.
 *
 * Minimal example
 *   size_t   cn;  void *c;
 *   squish_compress_alloc(data, n, &c, &cn);          // compress
 *   size_t   dn;  void *d;
 *   squish_decompress_alloc(c, cn, &d, &dn);          // decompress + verify
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
    SQUISH_E_FORMAT    = -3,  /* bad magic, bad mode byte, or truncated data */
    SQUISH_E_DSTSIZE   = -4,  /* dst buffer too small (see function docs)    */
    SQUISH_E_TOOBIG    = -5,  /* input larger than SQUISH_MAX_INPUT          */
    SQUISH_E_IO        = -6,  /* file open/read/write failed (file helpers)  */
    SQUISH_E_CHECKSUM  = -7   /* decompressed data failed integrity check    */
} squish_status;

/* Largest supported input, in bytes. */
#define SQUISH_MAX_INPUT ((uint64_t)0xFFFFFFF0u)

/* Chunk sizing for the multi-threaded functions. chunk_size = 0 selects the
 * default; anything smaller than the minimum is raised to it. */
#define SQUISH_DEFAULT_CHUNK ((size_t)16 << 20)   /* 16 MiB */
#define SQUISH_MIN_CHUNK     ((size_t)64 << 10)   /* 64 KiB */

/* --- introspection ------------------------------------------------------ */

/* Library version, e.g. "1.0.0". Static string, never NULL. */
SQUISH_API const char *squish_version(void);

/* Human-readable description of a squish_status. Static string, never NULL.*/
SQUISH_API const char *squish_strerror(int code);

/* --- sizing ------------------------------------------------------------- */

/* Worst-case compressed size for src_len input bytes. Guaranteed sufficient:
 * if dst has at least this capacity, squish_compress cannot fail with
 * SQUISH_E_DSTSIZE (incompressible data falls back to stored mode). */
SQUISH_API size_t squish_compress_bound(size_t src_len);

/* Read the original (decompressed) size from a compressed buffer header.
 * Needs only the first 12 bytes. Returns SQUISH_OK and sets *out_size, or
 * SQUISH_E_FORMAT / SQUISH_E_PARAM. */
SQUISH_API int squish_decompressed_size(const void *src, size_t src_len,
                                        uint64_t *out_size);

/* --- one-shot, caller-provided buffers ----------------------------------- */

/* Compress src[0..src_len) into dst.
 * In:  *dst_len = capacity of dst.
 * Out: *dst_len = compressed size.
 * Returns SQUISH_OK, or SQUISH_E_DSTSIZE if capacity was insufficient
 * (use squish_compress_bound), or SQUISH_E_PARAM/E_NOMEM/E_TOOBIG. */
SQUISH_API int squish_compress(const void *src, size_t src_len,
                               void *dst, size_t *dst_len);

/* Decompress src[0..src_len) into dst, verifying the checksum.
 * In:  *dst_len = capacity of dst.
 * Out: *dst_len = decompressed size.
 * If capacity is too small, returns SQUISH_E_DSTSIZE and sets *dst_len to
 * the required size (from the header) without writing to dst.
 * Returns SQUISH_OK, or E_FORMAT (corrupt container), E_CHECKSUM (payload
 * decoded but integrity check failed), E_PARAM, E_NOMEM. */
SQUISH_API int squish_decompress(const void *src, size_t src_len,
                                 void *dst, size_t *dst_len);

/* --- one-shot, library-allocated output ---------------------------------- */

/* As above, but the library allocates the output buffer (exactly sized for
 * decompress, trimmed for compress). Free with squish_free. On any error
 * *dst is set to NULL. */
SQUISH_API int squish_compress_alloc(const void *src, size_t src_len,
                                     void **dst, size_t *dst_len);
SQUISH_API int squish_decompress_alloc(const void *src, size_t src_len,
                                       void **dst, size_t *dst_len);

/* Free a buffer returned by the *_alloc functions. NULL is a no-op. */
SQUISH_API void squish_free(void *p);

/* --- whole-file convenience helpers -------------------------------------- */

/* Compress/decompress src_path into dst_path (overwritten if it exists).
 * Returns SQUISH_OK or any error above; SQUISH_E_IO covers file trouble. */
SQUISH_API int squish_compress_file(const char *src_path,
                                    const char *dst_path);
SQUISH_API int squish_decompress_file(const char *src_path,
                                      const char *dst_path);

/* Progress reporting. `processed` counts bytes of ORIGINAL (uncompressed)
 * data handled so far, out of `total`; invoked every 64 KiB of progress and
 * once with processed == total on completion. Called from the coding loop:
 * keep it cheap, and do not call back into the library from it. */
typedef void (*squish_progress_fn)(uint64_t processed, uint64_t total,
                                   void *user);

/* As squish_{,de}compress_file, additionally reporting progress through
 * `progress` (may be NULL, which is equivalent to the plain versions). */
SQUISH_API int squish_compress_file2(const char *src_path,
                                     const char *dst_path,
                                     squish_progress_fn progress, void *user);
SQUISH_API int squish_decompress_file2(const char *src_path,
                                       const char *dst_path,
                                       squish_progress_fn progress, void *user);

/* --- multi-threaded (multi-block "SQ01" streams) -------------------------- */

/* Number of processors online (>= 1). What nthreads = 0 selects below. */
SQUISH_API int squish_threads(void);

/* Compress src into an SQ01 multi-block stream: the input is split into
 * chunk_size-byte chunks (0 = SQUISH_DEFAULT_CHUNK), each compressed as an
 * independent SQ02 stream by a pool of nthreads workers (0 = all cores,
 * 1 = run serially — the output is identical either way; only chunk_size
 * shapes the stream). Inputs no larger than one chunk produce a plain SQ02
 * stream. Buffer/return contract of squish_compress, including the
 * squish_compress_bound guarantee (incompressible inputs fall back to a
 * single stored-mode SQ02 stream).
 *
 * `progress` (may be NULL) is called as documented for squish_progress_fn;
 * calls are serialized by the library but may come from worker threads. */
SQUISH_API int squish_compress_mt(const void *src, size_t src_len,
                                  void *dst, size_t *dst_len,
                                  int nthreads, size_t chunk_size,
                                  squish_progress_fn progress, void *user);

/* Decompress an SQ01 or SQ02 stream, chunks in parallel where the format
 * allows (nthreads as above). Buffer/return contract of squish_decompress. */
SQUISH_API int squish_decompress_mt(const void *src, size_t src_len,
                                    void *dst, size_t *dst_len,
                                    int nthreads,
                                    squish_progress_fn progress, void *user);

/* Library-allocated-output and whole-file variants of the two above. */
SQUISH_API int squish_compress_alloc_mt(const void *src, size_t src_len,
                                        void **dst, size_t *dst_len,
                                        int nthreads, size_t chunk_size,
                                        squish_progress_fn progress,
                                        void *user);
SQUISH_API int squish_decompress_alloc_mt(const void *src, size_t src_len,
                                          void **dst, size_t *dst_len,
                                          int nthreads,
                                          squish_progress_fn progress,
                                          void *user);
SQUISH_API int squish_compress_file_mt(const char *src_path,
                                       const char *dst_path,
                                       int nthreads, size_t chunk_size,
                                       squish_progress_fn progress,
                                       void *user);
SQUISH_API int squish_decompress_file_mt(const char *src_path,
                                         const char *dst_path,
                                         int nthreads,
                                         squish_progress_fn progress,
                                         void *user);

#ifdef __cplusplus
}
#endif
#endif /* SQUISH_H */
