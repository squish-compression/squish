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

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
