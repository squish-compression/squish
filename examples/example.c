/* Minimal libsquish usage: compress and restore a buffer.
 * Build (from repo root):  make example   — links against libsquish.so
 */
#include <squish.h>   /* or "squish.h" when building in-tree */

#include <stdio.h>
#include <string.h>

int main(void) {
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
