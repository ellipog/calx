// Parallel float sum: π by the midpoint rule, 4 / (1 + x²) over n strips, the classic OpenMP
// reduction example, summed on the given number of threads. C uses an OpenMP `reduction`, whose
// partial sums depend on the thread count; Calx gives each of 1,024 chunks its own sum, Rust uses
// Rayon's `sum`, and Go one partial sum per goroutine.
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    long long n = atoll(argv[1]);
    omp_set_num_threads(atoi(argv[2]));
    double step = 1.0 / (double)n, total = 0.0;
#pragma omp parallel for reduction(+ : total)
    for (long long i = 0; i < n; i++) {
        double x = ((double)i + 0.5) * step;
        total += 4.0 / (1.0 + x * x);
    }
    double pi = total * step;
    uint64_t bits;
    memcpy(&bits, &pi, sizeof bits);
    printf("%.6f\ninfo bits %016llx\n", pi, (unsigned long long)bits);
    return 0;
}
