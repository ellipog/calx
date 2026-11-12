// Parallel spectral-norm: the Benchmarks Game's power method at n = 5,500, each of its 40
// matrix-vector products split by rows across the given number of threads. Each element is summed
// in order by one thread, so every language gets the same digits at any thread count. Calx uses
// `parallel for`, Rust Rayon, Go goroutines and C OpenMP.
#include <math.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double a(size_t i, size_t j) {
    size_t ij = i + j;
    return 1.0 / (double)(ij * (ij + 1) / 2 + i + 1);
}

static void times(const double *v, double *out, long n, int transposed) {
#pragma omp parallel for
    for (long i = 0; i < n; i++) {
        double s = 0.0;
        for (long j = 0; j < n; j++) s += (transposed ? a((size_t)j, (size_t)i) : a((size_t)i, (size_t)j)) * v[j];
        out[i] = s;
    }
}

int main(int argc, char **argv) {
    long n = atol(argv[1]);
    omp_set_num_threads(atoi(argv[2]));
    double *u = malloc((size_t)n * sizeof(double)), *v = malloc((size_t)n * sizeof(double)),
           *tmp = malloc((size_t)n * sizeof(double));
    for (long i = 0; i < n; i++) u[i] = 1.0;
    for (int k = 0; k < 10; k++) {
        times(u, tmp, n, 0);
        times(tmp, v, n, 1);
        times(v, tmp, n, 0);
        times(tmp, u, n, 1);
    }
    double vbv = 0.0, vv = 0.0;
    for (long i = 0; i < n; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    double r = sqrt(vbv / vv);
    uint64_t bits;
    memcpy(&bits, &r, sizeof bits);
    printf("%.9f\ninfo bits %016llx\n", r, (unsigned long long)bits);
    free(u);
    free(v);
    free(tmp);
    return 0;
}
