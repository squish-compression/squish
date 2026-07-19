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

/* squish — command-line front end for libsquish.
 *
 * Everything squish writes is a SQUISH archive: `c` packs a file or a whole
 * directory tree into one, `d` unpacks it, and `l`/`x` inspect or pull a
 * single member without inflating the rest. There is a single on-disk format. */
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  define SQ_ISATTY(f) _isatty(_fileno(f))
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
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
        "usage: squish [-q] [-t N] [-b N] c input output    (compress)\n"
        "       squish [-q] [-t N]        d archive output   (decompress / extract)\n"
        "       squish l archive                             (list archive contents)\n"
        "       squish [-q] x archive member [dest]          (extract one file or dir)\n"
        "  -q, --quiet     no progress or summary; errors only\n"
        "  -t, --threads N use N threads, 0 = all cores (compress default: 1,\n"
        "                  which keeps the ratio-optimal single-block layout;\n"
        "                  decompress default: all cores)\n"
        "  -b, --block N   split members into N MiB blocks (default 16; smaller\n"
        "                  = more parallelism, slightly worse ratio)\n"
        "\n"
        "'input' may be a file or a directory; either way 'output' is a SQUISH\n"
        "archive. A directory is packed with every file as its own member, so 'l'\n"
        "lists it and 'x' pulls out a single file or subtree without inflating the\n"
        "rest; 'd' restores a single file, or recreates a whole tree under\n"
        "'output'.\n",
        squish_version());
    return 2;
}

/* Live status line, redrawn in place on stderr; only shown on a terminal so
 * redirected output doesn't fill with carriage returns. */
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

/* ---------------- small filesystem helpers ------------------------------- */
static int path_is_dir(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
#endif
}

/* malloc'd copy of `path` with trailing '/' and '\\' trimmed (never to empty).*/
static char *strip_trailing_sep(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, path, n); r[n] = '\0';
    return r;
}

/* Last path component, with no directory or drive prefix; never traverses. */
static const char *base_name(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    if (!*b || !strcmp(b, ".") || !strcmp(b, "..")) return "extracted.out";
    return b;
}

/* ============================ top-level commands ========================= */

/* `c`: compress a file or directory `in` into the archive `out`. */
static int compress_cmd(const char *in, const char *out, int threads,
                        size_t block, int quiet, squish_progress_fn cb,
                        status *st) {
    int is_dir = path_is_dir(in);
    char *disp = strip_trailing_sep(in);
    if (!disp) { fprintf(stderr, "squish: out of memory\n"); return 1; }

    double t0 = now_sec();
    int rc = is_dir
        ? squish_archive_create(disp, out, threads, block, cb, st)
        : squish_compress_file(disp, out, threads, block, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        free(disp);
        return 1;
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
        if (is_dir)
            fprintf(stderr,
                "%s/ -> %s: %llu entries, %llu -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
                disp, out, (unsigned long long)entries,
                (unsigned long long)total, outsz,
                total ? 8.0 * (double)outsz / (double)total : 0.0,
                dt > 0 && total ? (double)total / 1e6 / dt : 0.0);
        else
            fprintf(stderr,
                "%s -> %s: %llu -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
                disp, out, (unsigned long long)total, outsz,
                total ? 8.0 * (double)outsz / (double)total : 0.0,
                dt > 0 && total ? (double)total / 1e6 / dt : 0.0);
    }
    free(disp);
    return 0;
}

/* Extract a whole tree archive `in` under directory `out`. */
static int extract_tree(const char *in, const char *out, int quiet,
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

/* `d`: restore a single-file archive to the file `out`, or extract a packed
 * tree under the directory `out` — decided by the archive's SINGLE flag. */
static int decompress_cmd(const char *in, const char *out, int threads,
                          int quiet, squish_progress_fn cb, status *st) {
    squish_archive *a;
    int rc = squish_archive_open(in, &a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        return 1;
    }
    squish_archive_info info; squish_archive_info_get(a, &info);
    int single = (info.flags & 1u) != 0;
    squish_archive_close(a);

    if (!single) return extract_tree(in, out, quiet, cb, st);

    double t0 = now_sec();
    rc = squish_decompress_file(in, out, threads, cb, st);
    double dt = now_sec() - t0;
    clear_status(st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", in, squish_strerror(rc));
        return 1;
    }
    if (!quiet) {
        long long insz = file_size(in), outsz = file_size(out);
        fprintf(stderr, "%s -> %s: %lld -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
            in, out, insz, outsz,
            outsz > 0 ? 8.0 * (double)insz / (double)outsz : 0.0,
            dt > 0 && outsz > 0 ? (double)outsz / 1e6 / dt : 0.0);
    }
    return 0;
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
    printf("SQUISH archive v%u%s: %llu entries, %llu bytes uncompressed\n",
           info.version, (info.flags & 1u) ? " (single file)" : "",
           (unsigned long long)cnt, (unsigned long long)info.total_size);
    printf("  mode   %12s %12s  name\n", "size", "stored");
    for (uint64_t i = 0; i < cnt; i++) {
        squish_archive_entry e;
        if (squish_archive_stat(a, i, &e) != SQUISH_OK) continue;
        printf("  %04o   %12llu %12llu  %s%s\n",
               e.mode & 0777u, (unsigned long long)e.size,
               (unsigned long long)e.stored_size,
               e.path[0] ? e.path : "(data)", e.is_dir ? "/" : "");
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
        const char *outf = dest ? dest : base_name(member);
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

    if (npos != 3 || (cmd != 'c' && cmd != 'd')) return usage();
    int compress = (cmd == 'c');
    if (threads < 0) threads = compress ? 1 : 0;    /* 0 = all cores */

    status st = { compress ? "compressing" : "decompressing", now_sec(), -1 };
    squish_progress_fn cb = (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;

    return compress
        ? compress_cmd(pos[1], pos[2], threads, block, quiet, cb, &st)
        : decompress_cmd(pos[1], pos[2], threads, quiet, cb, &st);
}
