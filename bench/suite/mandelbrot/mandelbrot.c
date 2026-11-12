// mandelbrot: the Benchmarks Game's n by n bitmap of the Mandelbrot set, 50 iterations a pixel,
// one bit a pixel, eight to a byte, as a PBM holds it. It prints the bitmap's FNV-1a hash rather
// than the bitmap.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 200;
    size_t row_bytes = (n + 7) / 8;
    uint8_t *bits = calloc(row_bytes * n, 1);
    double size = (double)n;
    for (size_t y = 0; y < n; y++) {
        double ci = 2.0 * (double)y / size - 1.0;
        uint8_t acc = 0;
        int count = 0;
        size_t at = y * row_bytes;
        for (size_t x = 0; x < n; x++) {
            double cr = 2.0 * (double)x / size - 1.5;
            double zr = 0.0, zi = 0.0, tr = 0.0, ti = 0.0;
            for (int i = 0; i < 50 && tr + ti <= 4.0; i++) {
                zi = 2.0 * zr * zi + ci;
                zr = tr - ti + cr;
                tr = zr * zr;
                ti = zi * zi;
            }
            acc <<= 1;
            if (tr + ti <= 4.0) acc |= 1;
            count++;
            if (count == 8) {
                bits[at++] = acc;
                acc = 0;
                count = 0;
            } else if (x == n - 1) {
                bits[at++] = (uint8_t)(acc << (8 - n % 8));
                acc = 0;
                count = 0;
            }
        }
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < row_bytes * n; i++) h = (h ^ bits[i]) * 0x100000001b3ull;
    printf("P4 %zu %zu\nfnv1a %016llx\n", n, n, (unsigned long long)h);
    free(bits);
    return 0;
}
