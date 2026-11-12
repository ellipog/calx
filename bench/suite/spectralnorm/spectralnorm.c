// spectral-norm: the Benchmarks Game's power method for the spectral norm of an infinite matrix,
// cut to n by n, with 10 rounds of u = AᵀA v, v = AᵀA u. Every version sums in the same order.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double a(size_t i, size_t j) {
    size_t ij = i + j;
    return 1.0 / (double)(ij * (ij + 1) / 2 + i + 1);
}

static void times(const double *v, double *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double s = 0.0;
        for (size_t j = 0; j < n; j++) s += a(i, j) * v[j];
        out[i] = s;
    }
}

static void times_transposed(const double *v, double *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double s = 0.0;
        for (size_t j = 0; j < n; j++) s += a(j, i) * v[j];
        out[i] = s;
    }
}

static void times_ata(const double *v, double *out, double *tmp, size_t n) {
    times(v, tmp, n);
    times_transposed(tmp, out, n);
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 100;
    double *u = malloc(n * sizeof(double));
    double *v = malloc(n * sizeof(double));
    double *tmp = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++) u[i] = 1.0;
    for (int k = 0; k < 10; k++) {
        times_ata(u, v, tmp, n);
        times_ata(v, u, tmp, n);
    }
    double vbv = 0.0, vv = 0.0;
    for (size_t i = 0; i < n; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    printf("%.9f\n", sqrt(vbv / vv));
    free(u);
    free(v);
    free(tmp);
    return 0;
}
