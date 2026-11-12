// reverse-complement: the Benchmarks Game's sequence reversal. Each sequence of fasta's output is
// read whole, turned into its reverse complement, every base its pair and in the other order, and
// written 60 to a line under its header.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static char pair[256];

static void flush_sequence(char *seq, size_t len) {
    size_t i = 0, j = len;
    while (i + 1 < j) {
        j--;
        char t = pair[(unsigned char)seq[i]];
        seq[i] = pair[(unsigned char)seq[j]];
        seq[j] = t;
        i++;
    }
    if (i + 1 == j) seq[i] = pair[(unsigned char)seq[i]];
    for (size_t at = 0; at < len; at += 60) {
        size_t m = len - at < 60 ? len - at : 60;
        fwrite(seq + at, 1, m, stdout);
        fputc('\n', stdout);
    }
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char *from = "ACGTUMRWSYKVHDBN", *to = "TGCAAKYWSRMBDHVN";
    for (int i = 0; i < 256; i++) pair[i] = (char)i;
    for (int i = 0; from[i]; i++) {
        pair[(unsigned char)from[i]] = to[i];
        pair[(unsigned char)from[i] + 32] = to[i];
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size);
    if (fread(text, 1, size, f) != size) return 1;
    fclose(f);
    char *seq = malloc(size);
    size_t len = 0;
    for (size_t i = 0; i < size;) {
        char *nl = memchr(text + i, '\n', size - i);
        size_t end = nl ? (size_t)(nl - text) : size;
        size_t line_end = end > i && text[end - 1] == '\r' ? end - 1 : end;
        if (text[i] == '>') {
            if (len) flush_sequence(seq, len);
            len = 0;
            fwrite(text + i, 1, line_end - i, stdout);
            fputc('\n', stdout);
        } else {
            memcpy(seq + len, text + i, line_end - i);
            len += line_end - i;
        }
        i = end + 1;
    }
    if (len) flush_sequence(seq, len);
    free(seq);
    free(text);
    return 0;
}
