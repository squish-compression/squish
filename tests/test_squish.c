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

/* libsquish test suite: API contract, round-trips, error paths, robustness */
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* portable directory ops for the archive tests */
#if defined(_WIN32)
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define RMDIR(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0777)
#  define RMDIR(p) rmdir(p)
#endif

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

/* deterministic pseudo-random bytes (xorshift64*) */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint8_t rnd(void) {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (uint8_t)((rng * 0x2545F4914F6CDD1Dull) >> 56);
}

static struct {
    int calls, monotonic;
    uint64_t done, total;
} progress_state = { 0, 1, 0, 0 };

static void progress_probe(uint64_t processed, uint64_t total, void *user) {
    (void)user;
    if (processed < progress_state.done) progress_state.monotonic = 0;
    progress_state.calls++;
    progress_state.done = processed;
    progress_state.total = total;
}

static void wr_le(uint8_t *p, uint64_t v, int nb) {
    for (int i = 0; i < nb; i++) p[i] = (uint8_t)(v >> (8*i));
}

/* write n bytes to path (archive test fixtures) */
static void wfile(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { if (n) fwrite(d, 1, n, f); fclose(f); }
}
/* 1 if file `path` holds exactly d[0..n) */
static int file_is(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    size_t got = buf ? fread(buf, 1, n, f) : 0;
    int extra = fgetc(f);                 /* must be EOF: no trailing bytes */
    fclose(f);
    int ok = buf && got == n && extra == EOF && (n == 0 || memcmp(buf, d, n) == 0);
    free(buf);
    return ok;
}

static void roundtrip(const uint8_t *data, size_t n, const char *name) {
    void *c = NULL, *d = NULL;
    size_t cn = 0, dn = 0;
    int rc = squish_compress_alloc(data, n, &c, &cn);
    CHECK(rc == SQUISH_OK, name);
    if (rc != SQUISH_OK) return;
    CHECK(cn <= squish_compress_bound(n), "  within bound");
    uint64_t hdr_n = 0;
    CHECK(squish_decompressed_size(c, cn, &hdr_n) == SQUISH_OK && hdr_n == n,
          "  header size");
    rc = squish_decompress_alloc(c, cn, &d, &dn);
    CHECK(rc == SQUISH_OK && dn == n && (n == 0 || memcmp(data, d, n) == 0),
          "  bytes identical");
    squish_free(c); squish_free(d);
}

int main(void) {
    printf("libsquish %s\n", squish_version());
    CHECK(strcmp(squish_version(), SQUISH_VERSION_STRING) == 0, "version");
    CHECK(strcmp(squish_strerror(SQUISH_E_CHECKSUM),
                 "integrity check failed") == 0, "strerror");

    /* --- round-trips across shapes and sizes --- */
    roundtrip(NULL, 0, "roundtrip empty");
    { uint8_t b = 42; roundtrip(&b, 1, "roundtrip 1 byte"); }
    {
        static uint8_t z[100000];
        roundtrip(z, sizeof z, "roundtrip zeros 100k");
    }
    {
        static uint8_t r[262144];
        for (size_t i = 0; i < sizeof r; i++) r[i] = rnd();
        roundtrip(r, sizeof r, "roundtrip random 256k (stored fallback)");
    }
    {
        static uint8_t t[300000];
        const char *w[] = {"the ","quick ","brown ","fox ","jumps ","over "};
        size_t p = 0;
        while (p < sizeof t) {
            const char *s = w[rnd() % 6];
            for (const char *q = s; *q && p < sizeof t; q++) t[p++] = (uint8_t)*q;
        }
        roundtrip(t, sizeof t, "roundtrip repetitive text 300k");
    }
    {   /* record-structured: 13-byte rows */
        static uint8_t rec[130000];
        for (size_t i = 0; i < sizeof rec; i++)
            rec[i] = (uint8_t)((i % 13) * 7 + (i / 13) % 3);
        roundtrip(rec, sizeof rec, "roundtrip 13-byte records");
    }
    {   /* all byte values */
        static uint8_t all[65536];
        for (size_t i = 0; i < sizeof all; i++) all[i] = (uint8_t)(i & 255);
        roundtrip(all, sizeof all, "roundtrip byte ramp 64k");
    }

    /* --- error paths --- */
    {
        uint8_t buf[64];
        size_t bn = sizeof buf;
        CHECK(squish_compress(NULL, 5, buf, &bn) == SQUISH_E_PARAM,
              "compress NULL src");
        CHECK(squish_decompress(buf, 0, buf, &bn) == SQUISH_E_FORMAT,
              "decompress empty src");
        memset(buf, 'X', sizeof buf);
        bn = sizeof buf;
        CHECK(squish_decompress(buf, sizeof buf, buf, &bn) == SQUISH_E_FORMAT,
              "decompress garbage");
    }
    {
        const char *msg = "hello, squish!";
        uint8_t small[4];
        size_t sn = sizeof small;
        CHECK(squish_compress(msg, strlen(msg), small, &sn) == SQUISH_E_DSTSIZE,
              "compress dst too small");
    }
    {
        const char *msg = "The rain in Spain stays mainly in the plain. "
                          "The rain in Spain stays mainly in the plain.";
        void *c; size_t cn;
        CHECK(squish_compress_alloc(msg, strlen(msg), &c, &cn) == SQUISH_OK,
              "compress text");
        uint8_t tiny[4]; size_t tn = sizeof tiny;
        int rc = squish_decompress(c, cn, tiny, &tn);
        CHECK(rc == SQUISH_E_DSTSIZE && tn == strlen(msg),
              "decompress dst too small reports needed size");
        /* corrupt one payload byte -> checksum must catch it */
        ((uint8_t*)c)[cn/2] ^= 0x40;
        void *d; size_t dn;
        rc = squish_decompress_alloc(c, cn, &d, &dn);
        CHECK(rc == SQUISH_E_CHECKSUM && d == NULL,
              "corruption detected by checksum");
        /* truncated stream must fail cleanly, not crash */
        uint8_t out[256]; size_t on = sizeof out;
        rc = squish_decompress(c, cn / 2, out, &on);
        CHECK(rc != SQUISH_OK, "truncated stream rejected");
        squish_free(c);
    }
    {   /* file helpers */
        const char *tmp_in  = "tests/.t_in";
        const char *tmp_sq  = "tests/.t_sq";
        const char *tmp_out = "tests/.t_out";
        FILE *f = fopen(tmp_in, "wb");
        for (int i = 0; i < 50000; i++) fputc((i * 31) & 255, f);
        fclose(f);
        CHECK(squish_compress_file(tmp_in, tmp_sq) == SQUISH_OK,
              "compress_file");
        CHECK(squish_decompress_file(tmp_sq, tmp_out) == SQUISH_OK,
              "decompress_file");
        FILE *a = fopen(tmp_in, "rb"), *b = fopen(tmp_out, "rb");
        int same = 1, ca, cb;
        do { ca = fgetc(a); cb = fgetc(b); if (ca != cb) same = 0; }
        while (ca != EOF && cb != EOF);
        fclose(a); fclose(b);
        CHECK(same, "file round-trip identical");
        CHECK(squish_compress_file("tests/.does_not_exist", tmp_sq)
              == SQUISH_E_IO, "missing input file");
        remove(tmp_in); remove(tmp_sq); remove(tmp_out);
    }
    {   /* multi-threaded round-trip: chunked SQ01 stream, all readers */
        static uint8_t t[500000];
        const char *w[] = {"alpha ","beta ","gamma ","delta ","omega "};
        size_t p = 0;
        while (p < sizeof t) {
            const char *s = w[rnd() % 5];
            for (const char *q = s; *q && p < sizeof t; q++) t[p++] = (uint8_t)*q;
        }
        void *c = NULL; size_t cn = 0;
        int rc = squish_compress_alloc_mt(t, sizeof t, &c, &cn,
                                          4, SQUISH_MIN_CHUNK, NULL, NULL);
        CHECK(rc == SQUISH_OK, "compress_mt text 500k");
        CHECK(cn <= squish_compress_bound(sizeof t), "  within bound");
        CHECK(cn >= 4 && memcmp(c, "SQ01", 4) == 0, "  multi-block magic");
        uint64_t hdr_n = 0;
        CHECK(squish_decompressed_size(c, cn, &hdr_n) == SQUISH_OK &&
              hdr_n == sizeof t, "  header size");
        void *d = NULL; size_t dn = 0;
        rc = squish_decompress_alloc_mt(c, cn, &d, &dn, 4, NULL, NULL);
        CHECK(rc == SQUISH_OK && dn == sizeof t && memcmp(t, d, dn) == 0,
              "  mt decompress identical");
        squish_free(d); d = NULL; dn = 0;
        rc = squish_decompress_alloc(c, cn, &d, &dn);    /* plain API reads SQ01 */
        CHECK(rc == SQUISH_OK && dn == sizeof t && memcmp(t, d, dn) == 0,
              "  plain decompress reads SQ01");
        squish_free(d);
        /* stream must not depend on thread count */
        void *c1 = NULL; size_t c1n = 0;
        rc = squish_compress_alloc_mt(t, sizeof t, &c1, &c1n,
                                      1, SQUISH_MIN_CHUNK, NULL, NULL);
        CHECK(rc == SQUISH_OK && c1n == cn && memcmp(c, c1, cn) == 0,
              "  output independent of thread count");
        squish_free(c1);
        /* corrupting any chunk must fail the (per-chunk) checksum */
        ((uint8_t*)c)[cn - 3] ^= 0x40;
        rc = squish_decompress_alloc_mt(c, cn, &d, &dn, 4, NULL, NULL);
        CHECK(rc == SQUISH_E_CHECKSUM && d == NULL,
              "  chunk corruption detected");
        squish_free(c);
    }
    {   /* mt on random data: outer stored-mode fallback keeps the bound */
        static uint8_t r[300000];
        for (size_t i = 0; i < sizeof r; i++) r[i] = rnd();
        void *c = NULL; size_t cn = 0;
        int rc = squish_compress_alloc_mt(r, sizeof r, &c, &cn,
                                          0, SQUISH_MIN_CHUNK, NULL, NULL);
        CHECK(rc == SQUISH_OK && cn <= squish_compress_bound(sizeof r),
              "compress_mt random within bound");
        void *d = NULL; size_t dn = 0;
        rc = squish_decompress_alloc_mt(c, cn, &d, &dn, 0, NULL, NULL);
        CHECK(rc == SQUISH_OK && dn == sizeof r && memcmp(r, d, dn) == 0,
              "  round-trip identical");
        squish_free(c); squish_free(d);
    }
    {   /* small input through the mt API stays a plain SQ02 stream */
        const char *msg = "tiny input, one chunk";
        uint8_t buf[128]; size_t bn = sizeof buf;
        int rc = squish_compress_mt(msg, strlen(msg), buf, &bn,
                                    8, 0, NULL, NULL);
        CHECK(rc == SQUISH_OK && memcmp(buf, "SQ02", 4) == 0,
              "compress_mt small input emits SQ02");
        CHECK(squish_threads() >= 1, "squish_threads");
    }
    {   /* malformed containers: every case must fail cleanly, never crash */
        static uint8_t t[150000];
        for (size_t i = 0; i < sizeof t; i++)
            t[i] = (uint8_t)("squish stress "[i % 14]);
        void *c = NULL; size_t cn = 0;
        int rc = squish_compress_alloc_mt(t, sizeof t, &c, &cn,
                                          2, SQUISH_MIN_CHUNK, NULL, NULL);
        CHECK(rc == SQUISH_OK && cn > 21 && memcmp(c, "SQ01", 4) == 0,
              "malformed-container template");
        uint8_t *m = (uint8_t*)malloc(cn);
        void *d = NULL; size_t dn = 0;
        int ok = 1;

        /* chunk count out of range: 0 and huge */
        memcpy(m, c, cn); wr_le(m + 13, 0, 4);
        ok &= squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        memcpy(m, c, cn); wr_le(m + 13, 0xFFFFFFFFu, 4);
        ok &= squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        CHECK(ok, "  bad chunk count rejected");

        /* chunk table that does not tile the payload */
        memcpy(m, c, cn); wr_le(m + 17, 0, 4);
        ok = squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        memcpy(m, c, cn); wr_le(m + 17, 0xFFFFFFF0u, 4);
        ok &= squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        CHECK(ok, "  bad chunk table rejected");

        /* total size that disagrees with the chunks' own sizes */
        memcpy(m, c, cn); wr_le(m + 4, sizeof t + 1, 8);
        ok = squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        memcpy(m, c, cn); wr_le(m + 4, sizeof t - 1, 8);
        ok &= squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        CHECK(ok, "  header size mismatch rejected");

        /* size field past the format limit must be rejected up front */
        memcpy(m, c, cn); wr_le(m + 4, ~0ull, 8);
        uint64_t hn;
        ok = squish_decompressed_size(m, cn, &hn) == SQUISH_E_FORMAT;
        ok &= squish_decompress_alloc(m, cn, &d, &dn) != SQUISH_OK;
        CHECK(ok, "  oversize header rejected");

        /* nested SQ01 chunk and bad mode bytes */
        uint32_t k = ((uint8_t*)c)[13] | ((uint32_t)((uint8_t*)c)[14] << 8) |
                     ((uint32_t)((uint8_t*)c)[15] << 16) |
                     ((uint32_t)((uint8_t*)c)[16] << 24);
        memcpy(m, c, cn); memcpy(m + 17 + 4 * (size_t)k, "SQ01", 4);
        ok = squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        memcpy(m, c, cn); m[12] = 0;
        ok &= squish_decompress_alloc(m, cn, &d, &dn) == SQUISH_E_FORMAT;
        CHECK(ok, "  nested container / bad mode rejected");

        free(m); squish_free(c);
    }
    {   /* resource hardening: forged streams must fail fast, not chew CPU */
        void *d = NULL; size_t dn = 0;
        /* 17-byte stream claiming 16 MB of output with no payload: the
         * decoder must notice input starvation immediately instead of
         * decoding megabytes of filler noise before the checksum fails */
        uint8_t hdr[17];
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr, "SQ02", 4);
        wr_le(hdr + 4, 1u << 24, 8);
        CHECK(squish_decompress_alloc(hdr, sizeof hdr, &d, &dn)
              == SQUISH_E_FORMAT, "starved stream rejected early");
        /* SQ01 whose chunks all claim zero output: no encoder emits these,
         * and each would cost a full model setup for nothing */
        uint8_t mb[59];
        memset(mb, 0, sizeof mb);
        memcpy(mb, "SQ01", 4);
        mb[12] = 2;
        wr_le(mb + 13, 2, 4);                       /* k = 2            */
        wr_le(mb + 17, 17, 4); wr_le(mb + 21, 17, 4);   /* chunk sizes  */
        memcpy(mb + 25, "SQ02", 4); mb[25 + 12] = 1;    /* empty stored */
        memcpy(mb + 42, "SQ02", 4); mb[42 + 12] = 1;
        CHECK(squish_decompress_alloc(mb, sizeof mb, &d, &dn)
              == SQUISH_E_FORMAT, "zero-length chunks rejected");
    }
    {   /* progress-reporting file helpers */
        const char *tmp_in  = "tests/.t_in";
        const char *tmp_sq  = "tests/.t_sq";
        const char *tmp_out = "tests/.t_out";
        FILE *f = fopen(tmp_in, "wb");
        for (int i = 0; i < 200000; i++) fputc((i * 31) & 255, f);
        fclose(f);
        CHECK(squish_compress_file2(tmp_in, tmp_sq,
                                    progress_probe, &progress_state)
              == SQUISH_OK, "compress_file2");
        CHECK(progress_state.calls >= 2 && progress_state.monotonic &&
              progress_state.done == progress_state.total &&
              progress_state.total == 200000,
              "  compress progress reported");
        memset(&progress_state, 0, sizeof progress_state);
        progress_state.monotonic = 1;
        CHECK(squish_decompress_file2(tmp_sq, tmp_out,
                                      progress_probe, &progress_state)
              == SQUISH_OK, "decompress_file2");
        CHECK(progress_state.calls >= 2 && progress_state.monotonic &&
              progress_state.done == progress_state.total &&
              progress_state.total == 200000,
              "  decompress progress reported");
        remove(tmp_in); remove(tmp_sq); remove(tmp_out);
    }
    {   /* mt file helpers with aggregated progress */
        const char *tmp_in  = "tests/.t_in";
        const char *tmp_sq  = "tests/.t_sq";
        const char *tmp_out = "tests/.t_out";
        FILE *f = fopen(tmp_in, "wb");
        for (int i = 0; i < 300000; i++) fputc((i * 31) & 255, f);
        fclose(f);
        memset(&progress_state, 0, sizeof progress_state);
        progress_state.monotonic = 1;
        CHECK(squish_compress_file_mt(tmp_in, tmp_sq, 3, SQUISH_MIN_CHUNK,
                                      progress_probe, &progress_state)
              == SQUISH_OK, "compress_file_mt");
        CHECK(progress_state.calls >= 2 && progress_state.monotonic &&
              progress_state.done == progress_state.total &&
              progress_state.total == 300000,
              "  mt compress progress monotonic");
        memset(&progress_state, 0, sizeof progress_state);
        progress_state.monotonic = 1;
        CHECK(squish_decompress_file_mt(tmp_sq, tmp_out, 3,
                                        progress_probe, &progress_state)
              == SQUISH_OK, "decompress_file_mt");
        CHECK(progress_state.calls >= 2 && progress_state.monotonic &&
              progress_state.done == progress_state.total &&
              progress_state.total == 300000,
              "  mt decompress progress monotonic");
        FILE *a = fopen(tmp_in, "rb"), *b = fopen(tmp_out, "rb");
        int same = 1, ca, cb2;
        do { ca = fgetc(a); cb2 = fgetc(b); if (ca != cb2) same = 0; }
        while (ca != EOF && cb2 != EOF);
        fclose(a); fclose(b);
        CHECK(same, "  mt file round-trip identical");
        remove(tmp_in); remove(tmp_sq); remove(tmp_out);
    }

    {   /* seekable archive: create, header, list, single-member + subtree
         * extract, memory-backed open, and malformed-input rejection */
        const char *root = "tests/.t_arc";
        const char *arc  = "tests/.t_arc.sqar";

        /* fixture tree:
         *   .t_arc/a.txt              (compressible text)
         *   .t_arc/empty/             (empty directory, must survive)
         *   .t_arc/sub/b.bin          (pseudo-random)
         *   .t_arc/sub/deep/c.txt     (repetitive) */
        static uint8_t atxt[64], bbin[5000], ctxt[3000];
        for (size_t i = 0; i < sizeof atxt; i++) atxt[i] = (uint8_t)('a' + (i % 26));
        for (size_t i = 0; i < sizeof bbin; i++) bbin[i] = rnd();
        for (size_t i = 0; i < sizeof ctxt; i++) ctxt[i] = "SQUISH\n"[i % 7];
        MKDIR(root);
        MKDIR("tests/.t_arc/empty");
        MKDIR("tests/.t_arc/sub");
        MKDIR("tests/.t_arc/sub/deep");
        wfile("tests/.t_arc/a.txt", atxt, sizeof atxt);
        wfile("tests/.t_arc/sub/b.bin", bbin, sizeof bbin);
        wfile("tests/.t_arc/sub/deep/c.txt", ctxt, sizeof ctxt);

        int rc = squish_archive_create(root, arc, 1, 0, NULL, NULL);
        CHECK(rc == SQUISH_OK, "archive_create");

        squish_archive *A = NULL;
        CHECK(squish_archive_open(arc, &A) == SQUISH_OK && A, "archive_open");
        if (A) {
            squish_archive_info info;
            CHECK(squish_archive_info_get(A, &info) == SQUISH_OK &&
                  info.version == 2 && info.entry_count == 6 &&
                  info.total_size == sizeof atxt + sizeof bbin + sizeof ctxt,
                  "  header + entry count");
            CHECK(squish_archive_count(A) == 6, "  count");

            /* entries are pre-order, dirs before contents, siblings sorted:
             * a.txt, empty/, sub/, sub/b.bin, sub/deep/, sub/deep/c.txt */
            squish_archive_entry e;
            CHECK(squish_archive_stat(A, 0, &e) == SQUISH_OK &&
                  strcmp(e.path, "a.txt") == 0 && !e.is_dir &&
                  e.size == sizeof atxt, "  stat[0] = a.txt");
            CHECK(squish_archive_stat(A, 1, &e) == SQUISH_OK &&
                  strcmp(e.path, "empty") == 0 && e.is_dir && e.size == 0,
                  "  stat[1] = empty/ (dir)");

            uint64_t idx = 999;
            CHECK(squish_archive_find(A, "sub/b.bin", &idx) == SQUISH_OK,
                  "  find sub/b.bin");
            CHECK(squish_archive_find(A, "nope", NULL) == SQUISH_E_FORMAT,
                  "  find missing -> E_FORMAT");

            /* single-member extract reads only that member's stream */
            void *m = NULL; size_t mn = 0;
            CHECK(squish_archive_extract(A, idx, &m, &mn) == SQUISH_OK &&
                  mn == sizeof bbin && m && memcmp(m, bbin, mn) == 0,
                  "  extract b.bin bytes match");
            squish_free(m);
            CHECK(squish_archive_extract_path(A, "sub/deep/c.txt", &m, &mn)
                  == SQUISH_OK && mn == sizeof ctxt && memcmp(m, ctxt, mn) == 0,
                  "  extract_path c.txt bytes match");
            squish_free(m);

            /* a directory member cannot be extracted as data */
            uint64_t di = 1;
            CHECK(squish_archive_extract(A, di, &m, &mn) == SQUISH_E_PARAM &&
                  m == NULL, "  extract dir -> E_PARAM");

            /* extract one file to disk */
            CHECK(squish_archive_extract_to_file(A, "a.txt", "tests/.t_a") == SQUISH_OK &&
                  file_is("tests/.t_a", atxt, sizeof atxt), "  extract_to_file a.txt");
            remove("tests/.t_a");

            /* subtree extract recreates only sub/ under the destination root */
            CHECK(squish_archive_extract_subtree(A, "sub", "tests/.t_out", NULL, NULL)
                  == SQUISH_OK, "  extract_subtree sub");
            CHECK(file_is("tests/.t_out/sub/b.bin", bbin, sizeof bbin) &&
                  file_is("tests/.t_out/sub/deep/c.txt", ctxt, sizeof ctxt),
                  "  subtree files present");
            /* files outside the prefix were NOT written */
            CHECK(!file_is("tests/.t_out/a.txt", atxt, sizeof atxt),
                  "  subtree excluded a.txt");
            remove("tests/.t_out/sub/deep/c.txt");
            remove("tests/.t_out/sub/b.bin");
            RMDIR("tests/.t_out/sub/deep");
            RMDIR("tests/.t_out/sub");
            RMDIR("tests/.t_out");

            squish_archive_close(A);
        }

        /* memory-backed open over the same archive image */
        {
            FILE *f = fopen(arc, "rb");
            long sz = 0; uint8_t *img = NULL;
            if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
                     img = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
                     if (img) { if (fread(img, 1, (size_t)sz, f) != (size_t)sz) sz = 0; }
                     fclose(f); }
            squish_archive *M = NULL;
            CHECK(img && squish_archive_open_memory(img, (size_t)sz, &M) == SQUISH_OK && M,
                  "archive_open_memory");
            void *m = NULL; size_t mn = 0;
            CHECK(M && squish_archive_extract_path(M, "sub/b.bin", &m, &mn) == SQUISH_OK &&
                  mn == sizeof bbin && memcmp(m, bbin, mn) == 0,
                  "  memory extract matches");
            squish_free(m);
            /* probe agrees with the container; a plain stream is not an archive */
            CHECK(img && squish_archive_probe(img, (size_t)sz) == 1, "  probe archive");
            CHECK(squish_archive_probe("SQ02not-an-archive", 18) == 0, "  probe non-archive");
            squish_archive_close(M);
            /* a truncated image is rejected, not crashed */
            squish_archive *T = NULL;
            CHECK(!img || squish_archive_open_memory(img, 20, &T) == SQUISH_E_FORMAT,
                  "  truncated image rejected");
            free(img);
        }

        /* cleanup fixture tree (files then dirs, bottom-up) */
        remove(arc);
        remove("tests/.t_arc/a.txt");
        remove("tests/.t_arc/sub/b.bin");
        remove("tests/.t_arc/sub/deep/c.txt");
        RMDIR("tests/.t_arc/sub/deep");
        RMDIR("tests/.t_arc/sub");
        RMDIR("tests/.t_arc/empty");
        RMDIR("tests/.t_arc");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
