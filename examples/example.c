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

/* Minimal libsquish usage: compress and restore a buffer, and — given an
 * archive path — list it and pull out one member without inflating the rest.
 * Build (from repo root):  make example   — links against libsquish.so
 * Run:  ./examples/example [archive.sqar]
 */
#include <squish.h>   /* or "squish.h" when building in-tree */

#include <stdio.h>
#include <string.h>

/* List a seekable archive and extract its first file member. */
static int archive_demo(const char *path) {
    squish_archive *a;
    int rc = squish_archive_open(path, &a);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "open %s: %s\n", path, squish_strerror(rc));
        return 1;
    }
    squish_archive_info info;
    squish_archive_info_get(a, &info);
    printf("archive %s: v%u, %llu entries, %llu bytes uncompressed\n",
           path, info.version, (unsigned long long)info.entry_count,
           (unsigned long long)info.total_size);

    uint64_t first_file = info.entry_count;   /* sentinel: none found */
    for (uint64_t i = 0; i < info.entry_count; i++) {
        squish_archive_entry e;
        squish_archive_stat(a, i, &e);
        printf("  %04o %10llu  %s%s\n", e.mode & 0777u,
               (unsigned long long)e.size, e.path, e.is_dir ? "/" : "");
        if (!e.is_dir && first_file == info.entry_count) first_file = i;
    }

    if (first_file < info.entry_count) {
        void *buf; size_t len;
        rc = squish_archive_extract(a, first_file, &buf, &len);   /* only this member */
        if (rc == SQUISH_OK) {
            squish_archive_entry e;
            squish_archive_stat(a, first_file, &e);
            printf("extracted %s: %zu bytes\n", e.path, len);
            squish_free(buf);
        } else {
            fprintf(stderr, "extract: %s\n", squish_strerror(rc));
        }
    }
    squish_archive_close(a);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) return archive_demo(argv[1]);

    const char *text =
        "Context mixing turns compression into prediction: many models "
        "vote on every bit, a mixer learns whom to trust, and an "
        "arithmetic coder writes the surprise. "
        "Context mixing turns compression into prediction!";
    size_t n = strlen(text);

    void *comp; size_t comp_len;
    int rc = squish_compress_alloc(text, n, &comp, &comp_len);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "compress: %s\n", squish_strerror(rc));
        return 1;
    }
    printf("compressed %zu -> %zu bytes\n", n, comp_len);

    void *back; size_t back_len;
    rc = squish_decompress_alloc(comp, comp_len, &back, &back_len);
    if (rc != SQUISH_OK) {
        fprintf(stderr, "decompress: %s\n", squish_strerror(rc));
        return 1;
    }
    printf("restored %zu bytes, %s\n", back_len,
           back_len == n && memcmp(text, back, n) == 0
               ? "content verified" : "MISMATCH");

    squish_free(comp);
    squish_free(back);
    return 0;
}
