// Word count: 5 million words of text, about 30 MB, lowercased, split into runs of ASCII letters,
// counted in the language's hash map, and the 20 most common printed, ties by the word. C has no
// hash map, so this is one written here: open addressing on FNV-1a, keys pointing into the text,
// doubled at half full.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *word;
    uint32_t len;
    int32_t n;
} Slot;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
    return h;
}

static int by_count(const void *a, const void *b) {
    const Slot *x = a, *y = b;
    if (x->n != y->n) return y->n - x->n;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->word, y->word, n);
    return c ? c : (int)x->len - (int)y->len;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size);
    if (fread(text, 1, size, f) != size) return 1;
    fclose(f);
    for (size_t i = 0; i < size; i++)
        if (text[i] >= 'A' && text[i] <= 'Z') text[i] += 32;
    size_t cap = 1024, len = 0;
    Slot *slots = calloc(cap, sizeof(Slot));
    long long total = 0;
    for (size_t i = 0; i < size;) {
        while (i < size && (text[i] < 'a' || text[i] > 'z')) i++;
        size_t start = i;
        while (i < size && text[i] >= 'a' && text[i] <= 'z') i++;
        if (i == start) continue;
        total++;
        const char *w = text + start;
        uint32_t n = (uint32_t)(i - start);
        size_t at = fnv(w, n) & (cap - 1);
        while (slots[at].word && (slots[at].len != n || memcmp(slots[at].word, w, n) != 0)) at = (at + 1) & (cap - 1);
        if (slots[at].word) {
            slots[at].n++;
            continue;
        }
        slots[at] = (Slot){w, n, 1};
        if (++len * 2 > cap) {
            size_t bigger = cap * 2;
            Slot *grown = calloc(bigger, sizeof(Slot));
            for (size_t j = 0; j < cap; j++) {
                if (!slots[j].word) continue;
                size_t k = fnv(slots[j].word, slots[j].len) & (bigger - 1);
                while (grown[k].word) k = (k + 1) & (bigger - 1);
                grown[k] = slots[j];
            }
            free(slots);
            slots = grown;
            cap = bigger;
        }
    }
    Slot *top = malloc(len * sizeof(Slot));
    size_t m = 0;
    for (size_t j = 0; j < cap; j++)
        if (slots[j].word) top[m++] = slots[j];
    qsort(top, m, sizeof(Slot), by_count);
    printf("words %lld\ndistinct %zu\n", total, len);
    for (size_t j = 0; j < m && j < 20; j++) printf("%d %.*s\n", top[j].n, (int)top[j].len, top[j].word);
    free(top);
    free(slots);
    free(text);
    return 0;
}
