// fasta: the Benchmarks Game's DNA sequence generator. A repeated sequence of 2n bases, then 3n and
// 5n bases drawn from two weighted alphabets with its linear congruential generator, written 60 to
// a line. k-nucleotide, reverse-complement and regex-redux read what it writes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define WIDTH 60

static const char *ALU =
    "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCC"
    "GTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGC"
    "AGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA";

static unsigned last = 42;

static double next_random(void) {
    last = (last * 3877 + 29573) % 139968;
    return (double)last / 139968.0;
}

static void repeat(const char *seq, size_t n) {
    size_t len = strlen(seq), k = 0;
    char line[WIDTH + 1];
    while (n > 0) {
        size_t m = n < WIDTH ? n : WIDTH;
        for (size_t i = 0; i < m; i++) {
            line[i] = seq[k];
            if (++k == len) k = 0;
        }
        line[m] = '\n';
        fwrite(line, 1, m + 1, stdout);
        n -= m;
    }
}

static void random_fasta(const char *codes, const double *probs, size_t count, size_t n) {
    double cumulative[16];
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += probs[i];
        cumulative[i] = sum;
    }
    char line[WIDTH + 1];
    while (n > 0) {
        size_t m = n < WIDTH ? n : WIDTH;
        for (size_t j = 0; j < m; j++) {
            double r = next_random();
            size_t i = 0;
            while (i < count - 1 && r >= cumulative[i]) i++;
            line[j] = codes[i];
        }
        line[m] = '\n';
        fwrite(line, 1, m + 1, stdout);
        n -= m;
    }
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 1000;
    static const double iub[] = {0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02};
    static const double homo[] = {0.3029549426680, 0.1979883004921, 0.1975473066391, 0.3015094502008};
    fputs(">ONE Homo sapiens alu\n", stdout);
    repeat(ALU, n * 2);
    fputs(">TWO IUB ambiguity codes\n", stdout);
    random_fasta("acgtBDHKMNRSVWY", iub, 15, n * 3);
    fputs(">THREE Homo sapiens frequency\n", stdout);
    random_fasta("acgt", homo, 4, n * 5);
    return 0;
}
