// Building and taking apart text: a million CSV-like lines built onto one string with formatting,
// then each line split at its commas, a field uppercased and three joined with " | " onto a second
// string, the lines that name one user counted, and a word replaced through the whole result. C
// grows its buffers by doubling and formats with snprintf.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static void reserve(Buf *b, size_t more) {
    if (b->len + more <= b->cap) return;
    while (b->len + more > b->cap) b->cap = b->cap ? b->cap * 2 : 64;
    b->p = realloc(b->p, b->cap);
}

static void append(Buf *b, const char *s, size_t n) {
    reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

int main(int argc, char **argv) {
    long long n = atoll(argv[1]);
    const char *words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    Buf built = {0};
    for (long long i = 0; i < n; i++) {
        reserve(&built, 64);
        built.len += (size_t)snprintf(built.p + built.len, 64, "%lld,user%lld,%lld.%02lld,%s\n", i, i % 1000, i * 7 % 10007,
                                      i % 100, words[i % 5]);
    }
    Buf out = {0};
    int matches = 0;
    for (size_t at = 0; at < built.len;) {
        const char *line = built.p + at;
        const char *end = memchr(line, '\n', built.len - at);
        const char *fields[4];
        size_t lens[4];
        const char *f = line;
        for (int k = 0; k < 4; k++) {
            const char *comma = k < 3 ? memchr(f, ',', (size_t)(end - f)) : end;
            fields[k] = f;
            lens[k] = (size_t)(comma - f);
            f = comma + 1;
        }
        reserve(&out, lens[3]);
        for (size_t k = 0; k < lens[3]; k++) {
            char c = fields[3][k];
            out.p[out.len++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
        append(&out, " | ", 3);
        append(&out, fields[1], lens[1]);
        append(&out, " | ", 3);
        append(&out, fields[2], lens[2]);
        append(&out, "\n", 1);
        if (lens[1] == 6 && memcmp(fields[1], "user42", 6) == 0) matches++;
        at = (size_t)(end - built.p) + 1;
    }
    Buf replaced = {0};
    for (size_t at = 0; at < out.len;) {
        const char *hit = NULL;
        for (const char *s = out.p + at; s + 4 <= out.p + out.len; s++) {
            s = memchr(s, 'u', (size_t)(out.p + out.len - s));
            if (!s || s + 4 > out.p + out.len) break;
            if (memcmp(s, "user", 4) == 0) {
                hit = s;
                break;
            }
        }
        if (!hit) {
            append(&replaced, out.p + at, out.len - at);
            break;
        }
        append(&replaced, out.p + at, (size_t)(hit - (out.p + at)));
        append(&replaced, "u", 1);
        at = (size_t)(hit - out.p) + 4;
    }
    printf("built %zu\nout %zu\nmatches %d\nreplaced %zu\n", built.len, out.len, matches, replaced.len);
    free(built.p);
    free(out.p);
    free(replaced.p);
    return 0;
}
