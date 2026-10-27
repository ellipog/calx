// The C library's math functions on bench/math/math.calx's inputs and loops. Build with
// clang -O3 -march=x86-64-v3 -fno-builtin libm.c -lm, so the calls stay calls.
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif
static double *inputs(uint64_t s, int n, double lo, double hi) {
    double *xs = malloc(n * sizeof *xs);
    for (int i = 0; i < n; i++) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; xs[i] = lo + (hi - lo) * ((double)(s >> 11) * 1.1102230246251565e-16); }
    return xs;
}
#define TIME1(NAME, EXPR) { double best = 1e30, sum = 0; for (int r = 0; r < 7; r++) { double t0 = now(), acc = 0; for (int i = 0; i < n; i++) acc += EXPR; double t = now() - t0; if (t < best) best = t; sum = acc; } printf("%s: %.4f ns/call (sum %.17g)\n", NAME, best * 1e9 / n, sum); }
int main(void) {
    int n = 2000000;
    double *sm = inputs(2, n, -20, 20), *pos = inputs(3, n, 0, 1000), *base = inputs(4, n, 0, 10), *expo = inputs(5, n, -5, 5);
    TIME1("exp", exp(sm[i]));
    TIME1("log", log(pos[i]));
    TIME1("pow", pow(base[i], expo[i]));
    double *trig = inputs(1, n, -10, 10), *unit = inputs(6, n, -1, 1), *unit2 = inputs(7, n, -1, 1);
    TIME1("sin", sin(trig[i]));
    TIME1("cos", cos(trig[i]));
    TIME1("tan", tan(trig[i]));
    TIME1("atan2", atan2(unit[i], unit2[i]));
    TIME1("acos", acos(unit[i]));
}
