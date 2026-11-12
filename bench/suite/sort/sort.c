// Sorting: 5 million 64-bit integers and 5 million doubles read from binary files, and a million
// strings read a line each, each list sorted with the language's own sort and eleven evenly
// spaced elements printed. C's is qsort, which calls a comparison function for each pair.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *dir, const char *name, size_t *size) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) exit(1);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(*size + 1);
    if (fread(data, 1, *size, f) != *size) exit(1);
    fclose(f);
    data[*size] = 0;
    return data;
}

static int by_int(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int by_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int by_string(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    size_t isize, fsize, ssize;
    int64_t *ints = (int64_t *)read_file(argv[1], "ints.bin", &isize);
    double *floats = (double *)read_file(argv[1], "floats.bin", &fsize);
    char *text = read_file(argv[1], "strings.txt", &ssize);
    size_t ni = isize / 8, nf = fsize / 8, ns = 0, cap = 1024;
    char **strs = malloc(cap * sizeof(char *));
    for (char *p = text; *p;) {
        char *nl = strchr(p, '\n');
        if (ns == cap) strs = realloc(strs, (cap *= 2) * sizeof(char *));
        strs[ns++] = p;
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }

    qsort(ints, ni, sizeof(int64_t), by_int);
    qsort(floats, nf, sizeof(double), by_double);
    qsort(strs, ns, sizeof(char *), by_string);

    for (size_t k = 0; k <= 10; k++) {
        size_t i = k * ni / 10, f = k * nf / 10, s = k * ns / 10;
        if (i > ni - 1) i = ni - 1;
        if (f > nf - 1) f = nf - 1;
        if (s > ns - 1) s = ns - 1;
        printf("%lld %.6f %s\n", (long long)ints[i], floats[f], strs[s]);
    }
    free(strs);
    free(text);
    free(floats);
    free(ints);
    return 0;
}
