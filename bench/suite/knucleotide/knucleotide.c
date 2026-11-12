// k-nucleotide: the Benchmarks Game's hash-table test. The third sequence of fasta's output is read,
// every k-mer of it counted for each k, and the 1- and 2-mers' frequencies and five longer k-mers'
// counts printed. Each k-mer is the key, two bits a base in a 64-bit integer. C has no hash map,
// so this is the one a C programmer writes: open addressing, linear probing, a multiplicative
// hash, doubled at half full.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key;
    int32_t count;
    int32_t used;
} Slot;

typedef struct {
    Slot *slots;
    size_t cap, len;
} Table;

static size_t slot_of(uint64_t key, size_t cap) {
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
}

static void grow(Table *t) {
    size_t cap = t->cap * 2;
    Slot *slots = calloc(cap, sizeof(Slot));
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slots[i].used) continue;
        size_t j = slot_of(t->slots[i].key, cap);
        while (slots[j].used) j = (j + 1) & (cap - 1);
        slots[j] = t->slots[i];
    }
    free(t->slots);
    t->slots = slots;
    t->cap = cap;
}

static void add(Table *t, uint64_t key) {
    size_t i = slot_of(key, t->cap);
    while (t->slots[i].used) {
        if (t->slots[i].key == key) {
            t->slots[i].count++;
            return;
        }
        i = (i + 1) & (t->cap - 1);
    }
    t->slots[i].used = 1;
    t->slots[i].key = key;
    t->slots[i].count = 1;
    if (++t->len * 2 > t->cap) grow(t);
}

static int32_t get(const Table *t, uint64_t key) {
    size_t i = slot_of(key, t->cap);
    while (t->slots[i].used) {
        if (t->slots[i].key == key) return t->slots[i].count;
        i = (i + 1) & (t->cap - 1);
    }
    return 0;
}

static uint64_t code(char b) {
    switch (b) {
    case 'A': return 0;
    case 'C': return 1;
    case 'G': return 2;
    default: return 3;
    }
}

static Table count(const char *seq, size_t len, size_t k) {
    Table t = {calloc(16, sizeof(Slot)), 16, 0};
    uint64_t mask = (1ull << (2 * k)) - 1, key = 0;
    for (size_t i = 0; i + 1 < k; i++) key = (key << 2) | code(seq[i]);
    for (size_t i = k - 1; i < len; i++) {
        key = ((key << 2) | code(seq[i])) & mask;
        add(&t, key);
    }
    return t;
}

static int by_count(const void *a, const void *b) {
    const Slot *x = a, *y = b;
    if (x->count != y->count) return y->count - x->count;
    return x->key < y->key ? -1 : x->key > y->key;
}

static void frequencies(const char *seq, size_t len, size_t k) {
    Table t = count(seq, len, k);
    Slot *entries = malloc(t.len * sizeof(Slot));
    size_t n = 0;
    for (size_t i = 0; i < t.cap; i++)
        if (t.slots[i].used) entries[n++] = t.slots[i];
    qsort(entries, n, sizeof(Slot), by_count);
    double total = (double)(len - k + 1);
    for (size_t i = 0; i < n; i++) {
        char s[32];
        for (size_t j = 0; j < k; j++) s[j] = "ACGT"[(entries[i].key >> (2 * (k - 1 - j))) & 3];
        s[k] = 0;
        printf("%s %.3f\n", s, 100.0 * entries[i].count / total);
    }
    printf("\n");
    free(entries);
    free(t.slots);
}

static void occurrences(const char *seq, size_t len, const char *kmer) {
    size_t k = strlen(kmer);
    Table t = count(seq, len, k);
    uint64_t key = 0;
    for (size_t i = 0; i < k; i++) key = (key << 2) | code(kmer[i]);
    printf("%d\t%s\n", get(&t, key), kmer);
    free(t.slots);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t cap = 1 << 20, len = 0;
    char *seq = malloc(cap);
    char line[4096];
    int inside = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '>') {
            if (inside) break;
            inside = strncmp(line, ">THREE", 6) == 0;
        } else if (inside) {
            for (char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
                if (len == cap) seq = realloc(seq, cap *= 2);
                seq[len++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
            }
        }
    }
    fclose(f);
    frequencies(seq, len, 1);
    frequencies(seq, len, 2);
    const char *kmers[] = {"GGT", "GGTA", "GGTATT", "GGTATTTTAATT", "GGTATTTTAATTTATAGT"};
    for (int i = 0; i < 5; i++) occurrences(seq, len, kmers[i]);
    free(seq);
    return 0;
}
