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

/* squish — command-line front end for libsquish */
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  include <wchar.h>
#  define SQ_ISATTY(f) _isatty(_fileno(f))
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  define SQ_ISATTY(f) isatty(fileno(f))
#endif

/* wall clock: clock() sums CPU time across threads, useless with -t */
static double now_sec(void) {
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
#if defined(_WIN32)
    long long n = _ftelli64(f);     /* ftell is 32-bit here: wrong past 2 GiB */
#else
    long long n = ftello(f);
#endif
    fclose(f);
    return n;
}

static int usage(void) {
    fprintf(stderr,
        "SQUISH %s — context-mixing compressor\n"
        "usage: squish [-q] [-t N] c input output    (compress)\n"
        "       squish [-q] [-t N] d input output    (decompress / extract all)\n"
        "       squish [-q] [-t N] s input output    (make self-extracting archive)\n"
        "       squish l archive                     (list archive contents)\n"
        "       squish [-q] x archive member [dest]  (extract one file or dir)\n"
        "  -q, --quiet     no progress or summary; errors only\n"
        "  -t, --threads N use N threads, 0 = all cores (compress default: 1,\n"
        "                  which keeps the ratio-optimal single-block format;\n"
        "                  decompress default: all cores)\n"
        "  -b, --block N   with -t: split input into N MiB blocks (default 16;\n"
        "                  smaller = more parallelism, slightly worse ratio)\n"
        "\n"
        "'input' may be a file or a directory. A directory is packed into a\n"
        "seekable SQAR archive — every file its own stream — so 'l' lists it and\n"
        "'x' pulls out a single file or subtree without inflating the rest; 'd'\n"
        "recreates the whole tree under 'output'. 's' writes a self-extracting\n"
        "executable: running `output` (no squish needed) unpacks the embedded\n"
        "file or directory. On Windows, name it `*.exe`.\n",
        squish_version());
    return 2;
}

/* Live status line, redrawn in place on stderr; only shown on a terminal
 * so redirected output doesn't fill with carriage returns. */
typedef struct {
    const char *verb;
    double      t0;
    int         last_pct;   /* last percent drawn; -1 = nothing drawn yet */
} status;

static void draw_status(uint64_t done, uint64_t total, void *user) {
    status *st = (status *)user;
    int pct = total ? (int)(100.0 * (double)done / (double)total) : 100;
    if (pct == st->last_pct) return;
    st->last_pct = pct;
    double dt = now_sec() - st->t0;
    fprintf(stderr, "\r%s: %3d%%  %.1f / %.1f MB  %.2f MB/s ",
        st->verb, pct, (double)done / 1e6, (double)total / 1e6,
        dt > 0 ? (double)done / 1e6 / dt : 0.0);
    fflush(stderr);
}

static void clear_status(const status *st) {
    if (st->last_pct >= 0) fprintf(stderr, "\r%60s\r", "");
}

/* ============================ self-extracting ============================ *
 * A self-extracting archive is this CLI itself (used as a stub) followed by a
 * compressed payload, the stored original name, and a fixed trailer:
 *
 *     [ stub executable ][ payload ][ name ][ 32-byte trailer ]
 *      0                  off                 filesize - 32
 *
 * The trailer, read from the end of the file, is (all integers little-endian):
 *     magic[8]="SQSFX01\n" | payload_off u64 | payload_len u64 |
 *     name_len u32 | flags u32 (0)
 *
 * At start-up the CLI reads its own trailing 32 bytes: a valid trailer means
 * "I am an archive" and it extracts; otherwise it is the ordinary tool. The
 * CLI is statically linked, so an archive needs no libsquish at run time. */

#define SFX_MAGIC       "SQSFX01\n"
#define SFX_MAGIC_LEN   8u
#define SFX_TRAILER_LEN 32u          /* magic8 + off8 + len8 + name4 + flags4 */
#define SFX_MAX_NAME    4096u

static const char *g_argv0 = NULL;   /* for the /proc-less self-open fallback */

static void put_u32le(unsigned char *p, uint32_t v) {
    p[0]=(unsigned char)v;       p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static void put_u64le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8*i));
}
static uint32_t get_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t get_u64le(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8*i);
    return v;
}

/* 64-bit seek/tell (plain fseek/ftell are 32-bit on Windows). */
#if defined(_WIN32)
#  define sq_fseek64(f,o) _fseeki64((f),(o),SEEK_SET)
#  define sq_ftell64(f)   _ftelli64(f)
#else
#  define sq_fseek64(f,o) fseeko((f),(off_t)(o),SEEK_SET)
#  define sq_ftell64(f)   ftello(f)
#endif

/* Open the running executable for reading ("rb"), or NULL on failure. */
static FILE *open_self(void) {
#if defined(_WIN32)
    DWORD cap = 512;
    for (;;) {
        wchar_t *path = (wchar_t *)malloc(cap * sizeof *path);
        if (!path) return NULL;
        DWORD n = GetModuleFileNameW(NULL, path, cap);
        if (n == 0) { free(path); return NULL; }
        if (n < cap) { FILE *f = _wfopen(path, L"rb"); free(path); return f; }
        free(path);                          /* n == cap: truncated, grow */
        if (cap >= (1u << 20)) return NULL;
        cap *= 2;
    }
#else
    char buf[8192];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0 && (size_t)n < sizeof buf) {
        buf[n] = '\0';
        FILE *f = fopen(buf, "rb");
        if (f) return f;
    }
    /* Fallback for systems without /proc: argv[0] if it names a path. */
    return g_argv0 ? fopen(g_argv0, "rb") : NULL;
#endif
}

typedef struct {
    uint64_t  payload_off, payload_len;
    uint32_t  name_len;
    long long filesize;
} sfx_info;

/* True (and fills *info) if the running executable carries a valid trailer. */
static int sfx_probe(sfx_info *info) {
    FILE *f = open_self();
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long long sz = sq_ftell64(f);
    if (sz < (long long)SFX_TRAILER_LEN) { fclose(f); return 0; }
    unsigned char t[SFX_TRAILER_LEN];
    if (sq_fseek64(f, sz - (long long)SFX_TRAILER_LEN) != 0 ||
        fread(t, 1, SFX_TRAILER_LEN, f) != SFX_TRAILER_LEN) {
        fclose(f); return 0;
    }
    fclose(f);
    if (memcmp(t, SFX_MAGIC, SFX_MAGIC_LEN) != 0) return 0;
    uint64_t off  = get_u64le(t + 8);
    uint64_t plen = get_u64le(t + 16);
    uint32_t nlen = get_u32le(t + 24);
    if (off == 0 || plen == 0 || nlen > SFX_MAX_NAME) return 0;
    /* [stub off][payload plen][name nlen][trailer 32] must fill the file. */
    uint64_t tail = plen + (uint64_t)nlen + SFX_TRAILER_LEN;
    if (tail > (uint64_t)sz || off != (uint64_t)sz - tail) return 0;
    info->payload_off = off; info->payload_len = plen;
    info->name_len = nlen;   info->filesize = sz;
    return 1;
}

/* Read [off, off+len) of the running executable into a fresh buffer. */
static int sfx_read_range(uint64_t off, uint64_t len, unsigned char **out) {
    if (len > (uint64_t)(size_t)-1) return -1;
    FILE *f = open_self();
    if (!f) return -1;
    if (sq_fseek64(f, (long long)off) != 0) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc(len ? (size_t)len : 1);
    if (!buf) { fclose(f); return -1; }
    if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf;
    return 0;
}

/* Read a whole file into a fresh buffer (self for the stub, or the input). */
static int read_file_all(FILE *f, unsigned char **out, size_t *out_len) {
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long n = sq_ftell64(f);
    if (n < 0 || (unsigned long long)n > (size_t)-1) { fclose(f); return -1; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return -1; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf; *out_len = (size_t)n;
    return 0;
}

/* Last path component, with no directory or drive prefix; never traverses. */
static const char *sfx_basename(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    if (!*b || !strcmp(b, ".") || !strcmp(b, "..")) return "extracted.out";
    return b;
}

/* ============================ archive helpers =========================== *
 * Directory archives — create, list, and single-member/subtree extract — now
 * live in libsquish behind the squish_archive_* API (see squish.h and
 * docs/FORMAT.md §12), so DLL/SO consumers get them too. This CLI is a thin
 * front end over that API; only a couple of filesystem helpers remain here,
 * for tidy display names and single-file writes. */

/* malloc'd copy of `path` with trailing '/' and '\\' trimmed (never to
 * empty). NULL on OOM. */
static char *strip_trailing_sep(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, path, n); r[n] = '\0';
    return r;
}

/* Filesystem probes (Windows synthesizes mode bits; POSIX stat follows links). */
static int path_is_dir(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
#endif
}
static int path_is_regular(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISREG(s.st_mode);
#endif
}

/* Write `n` bytes to `path` (single-file output). 0 on ok, -1 on failure. */
static int write_file_bytes(const char *path, const unsigned char *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = !(n && fwrite(d, 1, n, f) != n);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(path); return -1; }
    return 0;
}

/* Build a self-extracting archive: stub || payload || name || trailer.
 *
 * The payload is either a single compressed stream (a plain file) or a whole
 * SQAR archive container (a directory) — the archive already holds per-file
 * streams, so it is embedded verbatim rather than re-compressed. sfx_run tells
 * the two apart by sniffing the container magic. */
static int sfx_create(const char *in_path, const char *out_path,
                      int threads, size_t block, int quiet,
                      squish_progress_fn cb, status *st) {
    unsigned char *stub; size_t stub_len;
    if (read_file_all(open_self(), &stub, &stub_len) != 0) {
        fprintf(stderr, "squish: cannot read own executable to use as SFX stub\n");
        return 1;
    }
    int is_dir = path_is_dir(in_path);
    char *in_disp = strip_trailing_sep(in_path);   /* tidy name + summary */
    if (!in_disp) { free(stub); fprintf(stderr, "squish: out of memory\n"); return 1; }

    double t0 = now_sec();
    void *payload = NULL; size_t payload_len = 0;
    uint64_t orig_total = 0;                        /* uncompressed size, for bpb */
    int rc = SQUISH_OK;
    if (is_dir) {
        /* Build the seekable archive to a scratch file, then embed its bytes. */
        size_t ol = strlen(out_path);
        char *tmp = (char *)malloc(ol + 8);
        if (!tmp) { free(stub); free(in_disp); fprintf(stderr, "squish: out of memory\n"); return 1; }
        memcpy(tmp, out_path, ol); memcpy(tmp + ol, ".sqtmp", 7);
        rc = squish_archive_create(in_disp, tmp, threads, block, cb, st);
        clear_status(st);
        if (rc == SQUISH_OK &&
            read_file_all(fopen(tmp, "rb"), (unsigned char **)&payload, &payload_len) != 0)
            rc = SQUISH_E_IO;
        remove(tmp);
        free(tmp);
        if (rc == SQUISH_OK) {                      /* read total for the summary */
            squish_archive *a;
            if (squish_archive_open_memory(payload, payload_len, &a) == SQUISH_OK) {
                squish_archive_info info;
                squish_archive_info_get(a, &info);
                orig_total = info.total_size;
                squish_archive_close(a);
            }
        }
    } else {
        unsigned char *in; size_t in_len;
        if (read_file_all(fopen(in_disp, "rb"), &in, &in_len) != 0) {
            free(stub); free(in_disp);
            fprintf(stderr, "squish: %s: %s\n", in_path, squish_strerror(SQUISH_E_IO));
            return 1;
        }
        rc = squish_compress_alloc_mt(in, in_len, &payload, &payload_len,
                                      threads, block, cb, st);
        clear_status(st);
        orig_total = in_len;
        free(in);
    }
    double dt = now_sec() - t0;
    if (rc != SQUISH_OK) {
        free(stub); free(in_disp); free(payload);
        fprintf(stderr, "squish: %s: %s\n", in_path, squish_strerror(rc));
        return 1;
    }

    const char *name = sfx_basename(in_disp);
    size_t name_len = strlen(name);
    if (name_len > SFX_MAX_NAME) name_len = SFX_MAX_NAME;

    unsigned char tr[SFX_TRAILER_LEN];
    memcpy(tr, SFX_MAGIC, SFX_MAGIC_LEN);
    put_u64le(tr + 8,  (uint64_t)stub_len);
    put_u64le(tr + 16, (uint64_t)payload_len);
    put_u32le(tr + 24, (uint32_t)name_len);
    put_u32le(tr + 28, 0);

    FILE *o = fopen(out_path, "wb");
    int ok = o != NULL;
    if (ok && stub_len    && fwrite(stub, 1, stub_len, o) != stub_len)          ok = 0;
    if (ok && payload_len && fwrite(payload, 1, payload_len, o) != payload_len) ok = 0;
    if (ok && name_len    && fwrite(name, 1, name_len, o) != name_len)          ok = 0;
    if (ok && fwrite(tr, 1, SFX_TRAILER_LEN, o) != SFX_TRAILER_LEN)             ok = 0;
    if (o && fclose(o) != 0) ok = 0;
    free(stub);
    free(in_disp);
    squish_free(payload);
    if (!ok) {
        fprintf(stderr, "squish: %s: %s\n", out_path, squish_strerror(SQUISH_E_IO));
        if (o) remove(out_path);
        return 1;
    }

#if !defined(_WIN32)
    chmod(out_path, 0755);           /* make the archive runnable */
#endif

    if (!quiet) {
        long long outsz = file_size(out_path);
        fprintf(stderr,
            "%s%s -> %s: %llu -> %lld bytes self-extracting"
            " (payload %llu, %.3f bpb, %.2f MB/s)\n",
            in_path, is_dir ? "/" : "", out_path,
            (unsigned long long)orig_total, outsz,
            (unsigned long long)payload_len,
            orig_total ? 8.0 * (double)payload_len / (double)orig_total : 0.0,
            dt > 0 && orig_total ? (double)orig_total / 1e6 / dt : 0.0);
    }
    return 0;
}

static int sfx_usage(void) {
    fprintf(stderr,
        "self-extracting SQUISH archive\n"
        "usage: %s [-f] [-q] [-t N] [output]\n"
        "  Extracts the embedded file (or directory tree) to the current\n"
        "  directory under its original name, or to <output> if given.\n"
        "  -f, --force      overwrite an existing output file\n"
        "  -q, --quiet      errors only\n"
        "  -t, --threads N  worker threads, 0 = all cores (default)\n",
        g_argv0 && *g_argv0 ? g_argv0 : "archive");
    return 2;
}

/* Run when the executable is itself a self-extracting archive. */
static int sfx_run(int argc, char **argv, const sfx_info *info) {
    int quiet = 0, force = 0, threads = 0;   /* extract: all cores by default */
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            sfx_usage(); return 0;
        }
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) {
            char *end;
            if (++i >= argc) return sfx_usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 0 || v > 4096) return sfx_usage();
            threads = (int)v;
        }
        else if (argv[i][0] == '-' && argv[i][1]) return sfx_usage();
        else if (!target) target = argv[i];
        else return sfx_usage();
    }

    /* Stored original name -> a safe basename we may extract to. */
    unsigned char *namebuf;
    uint64_t name_off = (uint64_t)info->filesize - SFX_TRAILER_LEN - info->name_len;
    if (sfx_read_range(name_off, info->name_len, &namebuf) != 0) {
        fprintf(stderr, "squish: cannot read embedded name\n");
        return 1;
    }
    char stored[SFX_MAX_NAME + 1];
    size_t nl = info->name_len;              /* <= SFX_MAX_NAME (probe checked) */
    memcpy(stored, namebuf, nl);
    stored[nl] = '\0';
    free(namebuf);
    const char *out = target ? target : sfx_basename(stored);

    /* Guard single-file overwrite (an archive payload merges into the target
     * directory instead, so only a colliding *file* is a conflict here). */
    if (!force && path_is_regular(out)) {
        fprintf(stderr, "squish: %s already exists (use -f to overwrite)\n", out);
        return 1;
    }

    unsigned char *payload;
    if (sfx_read_range(info->payload_off, info->payload_len, &payload) != 0) {
        fprintf(stderr, "squish: cannot read embedded payload\n");
        return 1;
    }

    status st = { "extracting", now_sec(), -1 };
    squish_progress_fn cb = (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;
    double t0, dt;

    /* A directory payload is a whole SQAR archive; unpack it under `out`. A
     * plain payload is one compressed stream; inflate it to a single file. */
    if (squish_archive_probe(payload, (size_t)info->payload_len)) {
        squish_archive *a;
        int rc = squish_archive_open_memory(payload, (size_t)info->payload_len, &a);
        if (rc != SQUISH_OK) {
            free(payload);
            fprintf(stderr, "squish: %s\n", squish_strerror(rc));
            return 1;
        }
        t0 = now_sec();
        rc = squish_archive_extract_subtree(a, NULL, out, cb, &st);
        dt = now_sec() - t0;
        clear_status(&st);
        squish_archive_info in2; squish_archive_info_get(a, &in2);
        uint64_t nf = 0, nb = in2.total_size, cnt = squish_archive_count(a);
        for (uint64_t i = 0; i < cnt; i++) {
            squish_archive_entry e;
            if (squish_archive_stat(a, i, &e) == SQUISH_OK && !e.is_dir) nf++;
        }
        squish_archive_close(a);
        free(payload);
        if (rc != SQUISH_OK) {
            fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(rc));
            return 1;
        }
        if (!quiet)
            fprintf(stderr, "extracted %s/: %llu files, %llu bytes (%.2f MB/s)\n",
                    out, (unsigned long long)nf, (unsigned long long)nb,
                    dt > 0 && nb ? (double)nb / 1e6 / dt : 0.0);
        return 0;
    }

    t0 = now_sec();
    void *outbuf; size_t outn;
    int rc = squish_decompress_alloc_mt(payload, (size_t)info->payload_len,
                                        &outbuf, &outn, threads, cb, &st);
    dt = now_sec() - t0;
    clear_status(&st);
    free(payload);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s\n", squish_strerror(rc));
        return 1;
    }

    FILE *o = fopen(out, "wb");
    int ok = o != NULL;
    if (ok && outn && fwrite(outbuf, 1, outn, o) != outn) ok = 0;
    if (o && fclose(o) != 0) ok = 0;
    squish_free(outbuf);
    if (!ok) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(SQUISH_E_IO));
        if (o) remove(out);
        return 1;
    }

    if (!quiet)
        fprintf(stderr, "extracted %s: %llu bytes (%.2f MB/s)\n",
                out, (unsigned long long)outn,
                dt > 0 && outn ? (double)outn / 1e6 / dt : 0.0);
    return 0;
}

/* ============================ top-level commands ========================= */

/* Compress directory `dir` into a seekable archive at `out`. */
static int compress_dir_cmd(const char *dir, const char *out, int threads,
                            size_t block, int quiet, squish_progress_fn cb,
                            status *st) {
    char *d = strip_trailing_sep(dir);
    if (!d) { fprintf(stderr, "squish: out of memory\n"); return 1; }
    double t0 = now_sec();
    int rc = squish_archive_create(d, out, threads, block, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", d, squish_strerror(rc));
        free(d); return 1;
    }
    if (!quiet) {
        uint64_t entries = 0, total = 0;
        squish_archive *a;
        if (squish_archive_open(out, &a) == SQUISH_OK) {
            squish_archive_info info;
            squish_archive_info_get(a, &info);
            entries = info.entry_count; total = info.total_size;
            squish_archive_close(a);
        }
        long long outsz = file_size(out);
        fprintf(stderr,
            "%s/ -> %s: %llu entries, %llu -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
            d, out, (unsigned long long)entries,
            (unsigned long long)total, outsz,
            total ? 8.0 * (double)outsz / (double)total : 0.0,
            dt > 0 && total ? (double)total / 1e6 / dt : 0.0);
    }
    free(d);
    return 0;
}

/* Extract the whole archive `in` under directory `out` (only what is needed is
 * inflated, member by member). */
static int extract_all_cmd(const char *in, const char *out, int quiet,
                           squish_progress_fn cb, status *st) {
    squish_archive *a;
    int rc = squish_archive_open(in, &a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        return 1;
    }
    double t0 = now_sec();
    rc = squish_archive_extract_subtree(a, NULL, out, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(rc));
        squish_archive_close(a);
        return 1;
    }
    if (!quiet) {
        squish_archive_info info; squish_archive_info_get(a, &info);
        uint64_t nf = 0, cnt = squish_archive_count(a);
        for (uint64_t i = 0; i < cnt; i++) {
            squish_archive_entry e;
            if (squish_archive_stat(a, i, &e) == SQUISH_OK && !e.is_dir) nf++;
        }
        fprintf(stderr, "%s -> %s/: %llu files, %llu bytes (%.2f MB/s)\n",
            in, out, (unsigned long long)nf, (unsigned long long)info.total_size,
            dt > 0 && info.total_size ? (double)info.total_size / 1e6 / dt : 0.0);
    }
    squish_archive_close(a);
    return 0;
}

/* Decompress a plain file `in` to `out` (single compressed stream). */
static int decompress_file_cmd(const char *in, const char *out, int threads,
                               int quiet, squish_progress_fn cb, status *st) {
    unsigned char *comp; size_t comp_len;
    if (read_file_all(fopen(in, "rb"), &comp, &comp_len) != 0) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(SQUISH_E_IO));
        return 1;
    }
    double t0 = now_sec();
    void *buf; size_t n;
    int rc = squish_decompress_alloc_mt(comp, comp_len, &buf, &n,
                                        threads, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    free(comp);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        return 1;
    }
    int wr = write_file_bytes(out, (const unsigned char *)buf, n);
    squish_free(buf);
    if (wr != 0) {
        fprintf(stderr, "squish: %s: %s\n", out, squish_strerror(SQUISH_E_IO));
        return 1;
    }
    if (!quiet) {
        long long insz = file_size(in);
        fprintf(stderr, "%s -> %s: %lld -> %llu bytes (%.3f bpb, %.2f MB/s)\n",
            in, out, insz, (unsigned long long)n,
            n ? 8.0 * (double)insz / (double)n : 0.0,
            dt > 0 && n ? (double)n / 1e6 / dt : 0.0);
    }
    return 0;
}

/* `d`: extract a whole archive, or decompress a plain file — decided by
 * sniffing the container magic in the first bytes. */
static int decompress_cmd(const char *in, const char *out, int threads,
                          int quiet, squish_progress_fn cb, status *st) {
    unsigned char head[12]; size_t got = 0;
    FILE *pf = fopen(in, "rb");
    if (pf) { got = fread(head, 1, sizeof head, pf); fclose(pf); }
    if (got >= 12 && squish_archive_probe(head, got))
        return extract_all_cmd(in, out, quiet, cb, st);
    return decompress_file_cmd(in, out, threads, quiet, cb, st);
}

/* `l`: print the archive header and one line per member. */
static int list_cmd(const char *arc_path) {
    squish_archive *a;
    int rc = squish_archive_open(arc_path, &a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", arc_path, squish_strerror(rc));
        return 1;
    }
    squish_archive_info info; squish_archive_info_get(a, &info);
    uint64_t cnt = squish_archive_count(a);
    printf("SQAR v%u: %llu entries, %llu bytes uncompressed\n",
           info.version, (unsigned long long)cnt,
           (unsigned long long)info.total_size);
    printf("  mode   %12s %12s  name\n", "size", "stored");
    for (uint64_t i = 0; i < cnt; i++) {
        squish_archive_entry e;
        if (squish_archive_stat(a, i, &e) != SQUISH_OK) continue;
        printf("  %04o   %12llu %12llu  %s%s\n",
               e.mode & 0777u, (unsigned long long)e.size,
               (unsigned long long)e.stored_size, e.path, e.is_dir ? "/" : "");
    }
    squish_archive_close(a);
    return 0;
}

/* `x`: extract one member — a file to `dest` (or its basename), or a directory
 * subtree under `dest` (or the current directory). */
static int extract_cmd(const char *arc_path, const char *member,
                       const char *dest, int quiet) {
    squish_archive *a;
    int rc = squish_archive_open(arc_path, &a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", arc_path, squish_strerror(rc));
        return 1;
    }
    uint64_t idx;
    if (squish_archive_find(a, member, &idx) != SQUISH_OK) {
        fprintf(stderr, "squish: %s: no such member in %s\n", member, arc_path);
        squish_archive_close(a);
        return 1;
    }
    squish_archive_entry e; squish_archive_stat(a, idx, &e);
    status st = { "extracting", now_sec(), -1 };
    squish_progress_fn cb = (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;
    if (e.is_dir) {
        const char *root = dest ? dest : ".";
        rc = squish_archive_extract_subtree(a, member, root, cb, &st);
        clear_status(&st);
        if (rc == SQUISH_OK && !quiet)
            fprintf(stderr, "extracted %s/ -> %s/\n", member, root);
    } else {
        const char *outf = dest ? dest : sfx_basename(member);
        rc = squish_archive_extract_to_file(a, member, outf);
        if (rc == SQUISH_OK && !quiet)
            fprintf(stderr, "extracted %s -> %s: %llu bytes\n",
                    member, outf, (unsigned long long)e.size);
    }
    squish_archive_close(a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", member, squish_strerror(rc));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    g_argv0 = argv[0];
    { sfx_info sfxi; if (sfx_probe(&sfxi)) return sfx_run(argc, argv, &sfxi); }

    int quiet = 0, threads = -1;    /* -1 = unset: per-direction default */
    size_t block = 0;               /* 0 = library default */
    const char *pos[4] = {0};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads")) {
            char *end;
            if (++i >= argc) return usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 0 || v > 4096) return usage();
            threads = (int)v;
        }
        else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--block")) {
            char *end;
            if (++i >= argc) return usage();
            long v = strtol(argv[i], &end, 10);
            if (*end || v < 1 || v > 4096) return usage();
            if ((unsigned long)v > ((size_t)-1 >> 20)) return usage();
            block = (size_t)v << 20;    /* MiB -> bytes, shift can't wrap */
        }
        else if (argv[i][0] == '-' && argv[i][1]) return usage();
        else if (npos < 4) pos[npos++] = argv[i];
        else return usage();
    }
    if (npos < 1 || pos[0][1]) return usage();
    char cmd = pos[0][0];

    /* list and extract-member have their own arities and no compress knobs */
    if (cmd == 'l') return npos == 2 ? list_cmd(pos[1]) : usage();
    if (cmd == 'x')
        return (npos == 3 || npos == 4)
            ? extract_cmd(pos[1], pos[2], npos == 4 ? pos[3] : NULL, quiet)
            : usage();

    if (npos != 3 || (cmd != 'c' && cmd != 'd' && cmd != 's')) return usage();
    int compress = (cmd == 'c');
    int sfx      = (cmd == 's');
    if (threads < 0) threads = (compress || sfx) ? 1 : 0;    /* 0 = all cores */

    status st = { compress || sfx ? "compressing" : "decompressing",
                  now_sec(), -1 };
    squish_progress_fn cb =
        (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;

    if (sfx) return sfx_create(pos[1], pos[2], threads, block, quiet, cb, &st);

    if (!compress)     /* decompress auto-detects a plain file vs. an archive */
        return decompress_cmd(pos[1], pos[2], threads, quiet, cb, &st);

    /* A directory input is packed into a seekable archive; a file is unchanged
     * (a plain SQ02/SQ01 stream, byte-for-byte as before). */
    if (path_is_dir(pos[1]))
        return compress_dir_cmd(pos[1], pos[2], threads, block, quiet, cb, &st);

    double t0 = now_sec();
    int rc = (threads == 1 && !block)
        ? squish_compress_file2(pos[1], pos[2], cb, &st)
        : squish_compress_file_mt(pos[1], pos[2], threads, block, cb, &st);
    double dt = now_sec() - t0;
    clear_status(&st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", pos[1], squish_strerror(rc));
        return 1;
    }
    if (quiet) return 0;
    long long in = file_size(pos[1]), out = file_size(pos[2]);
    fprintf(stderr, "%s -> %s: %lld -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
        pos[1], pos[2], in, out,
        in > 0 ? 8.0 * (double)out / (double)in : 0.0,
        dt > 0 && in > 0 ? (double)in / 1e6 / dt : 0.0);
    return 0;
}
