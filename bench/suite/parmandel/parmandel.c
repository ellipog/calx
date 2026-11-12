// Parallel mandelbrot: the Benchmarks Game's bitmap, 6,000 by 6,000, its rows computed at once on
// the given number of threads, and the bitmap's FNV-1a hash printed. Calx uses `parallel for`,
// Rust Rayon, Go goroutines and C OpenMP, here with a dynamic schedule, since rows differ in cost.
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void row(size_t y, size_t n, uint8_t *bits) {
    double size = (double)n;
    double ci = 2.0 * (double)y / size - 1.0;
    uint8_t acc = 0;
    int count = 0;
    size_t at = 0;
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
            bits[at] = (uint8_t)(acc << (8 - n % 8));
        }
    }
}

int main(int argc, char **argv) {
    long n = atol(argv[1]);
    omp_set_num_threads(atoi(argv[2]));
    size_t row_bytes = ((size_t)n + 7) / 8;
    uint8_t *bits = calloc(row_bytes * (size_t)n, 1);
#pragma omp parallel for schedule(dynamic)
    for (long y = 0; y < n; y++) row((size_t)y, (size_t)n, bits + (size_t)y * row_bytes);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < row_bytes * (size_t)n; i++) h = (h ^ bits[i]) * 0x100000001b3ull;
    printf("P4 %ld %ld\nfnv1a %016llx\n", n, n, (unsigned long long)h);
    free(bits);
    return 0;
}
