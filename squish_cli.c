/* squish — command-line front end for libsquish */
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc != 4 || (argv[1][0] != 'c' && argv[1][0] != 'd') || argv[1][1]) {
        fprintf(stderr,
            "SQUISH %s — context-mixing compressor\n"
            "usage: squish c input output   (compress)\n"
            "       squish d input output   (decompress)\n",
            squish_version());
        return 2;
    }
    int compress = (argv[1][0] == 'c');
    clock_t t0 = clock();
    int rc = compress ? squish_compress_file(argv[2], argv[3])
                      : squish_decompress_file(argv[2], argv[3]);
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (rc != SQUISH_OK) {
        fprintf(stderr, "squish: %s: %s\n", argv[2], squish_strerror(rc));
        return 1;
    }
    long long in = file_size(argv[2]), out = file_size(argv[3]);
    long long data = compress ? in : out;
    fprintf(stderr, "%s -> %s: %lld -> %lld bytes (%.3f bpb, %.2f MB/s)\n",
        argv[2], argv[3], in, out,
        data > 0 ? 8.0 * (double)(compress ? out : in) / (double)data : 0.0,
        dt > 0 && data > 0 ? (double)data / 1e6 / dt : 0.0);
    return 0;
}
