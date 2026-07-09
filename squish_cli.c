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

#if defined(_WIN32)
#  include <io.h>
#  define SQ_ISATTY(f) _isatty(_fileno(f))
#else
#  include <unistd.h>
#  define SQ_ISATTY(f) isatty(fileno(f))
#endif

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

static int usage(void) {
    fprintf(stderr,
        "SQUISH %s — context-mixing compressor\n"
        "usage: squish [-q] c input output   (compress)\n"
        "       squish [-q] d input output   (decompress)\n"
        "  -q, --quiet   no progress or summary; errors only\n",
        squish_version());
    return 2;
}

/* Live status line, redrawn in place on stderr; only shown on a terminal
 * so redirected output doesn't fill with carriage returns. */
typedef struct {
    const char *verb;
    clock_t     t0;
    int         last_pct;   /* last percent drawn; -1 = nothing drawn yet */
} status;

static void draw_status(uint64_t done, uint64_t total, void *user) {
    status *st = (status *)user;
    int pct = total ? (int)(100.0 * (double)done / (double)total) : 100;
    if (pct == st->last_pct) return;
    st->last_pct = pct;
    double dt = (double)(clock() - st->t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "\r%s: %3d%%  %.1f / %.1f MB  %.2f MB/s ",
        st->verb, pct, (double)done / 1e6, (double)total / 1e6,
        dt > 0 ? (double)done / 1e6 / dt : 0.0);
    fflush(stderr);
}

static void clear_status(const status *st) {
    if (st->last_pct >= 0) fprintf(stderr, "\r%60s\r", "");
}

int main(int argc, char **argv) {
    int quiet = 0;
    const char *pos[3] = {0};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1]) return usage();
        else if (npos < 3) pos[npos++] = argv[i];
        else return usage();
    }
    if (npos != 3 || (pos[0][0] != 'c' && pos[0][0] != 'd') || pos[0][1])
        return usage();
    int compress = (pos[0][0] == 'c');

    status st = { compress ? "compressing" : "decompressing", clock(), -1 };
    squish_progress_fn cb =
        (!quiet && SQ_ISATTY(stderr)) ? draw_status : NULL;

    clock_t t0 = clock();
    int rc = compress
        ? squish_compress_file2(pos[1], pos[2], cb, &st)
        : squish_decompress_file2(pos[1], pos[2], cb, &st);
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    clear_status(&st);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", pos[1], squish_strerror(rc));
        return 1;
    }
    if (quiet) return 0;
    long long in = file_size(pos[1]), out = file_size(pos[2]);
    long long data = compress ? in : out;
    fprintf(stderr, "%s -> %s: %lld -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
        pos[1], pos[2], in, out,
        data > 0 ? 8.0 * (double)(compress ? out : in) / (double)data : 0.0,
        dt > 0 && data > 0 ? (double)data / 1e6 / dt : 0.0);
    return 0;
}
