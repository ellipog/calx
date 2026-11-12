// Hash maps: 2 million integer keys inserted, 4 million looked up, half of them missing, every
// other key removed and all looked up again, and the values summed; then 500,000 string keys
// inserted and a million looked up. C has no hash map, so these are written here: open
// addressing with linear probing, a multiplicative hash for integers and FNV-1a for strings,
// doubled at half full, and removal leaving a tombstone.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EMPTY, FULL, GONE };

typedef struct {
    int64_t key, value;
    int state;
} Slot;

typedef struct {
    Slot *slots;
    size_t cap, len, used;
} IntMap;

static size_t home(int64_t key, size_t cap) { return (size_t)(((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1); }

static void int_grow(IntMap *m) {
    size_t cap = m->len * 4 > m->cap ? m->cap * 2 : m->cap;
    Slot *slots = calloc(cap, sizeof(Slot));
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].state != FULL) continue;
        size_t j = home(m->slots[i].key, cap);
        while (slots[j].state == FULL) j = (j + 1) & (cap - 1);
        slots[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = slots;
    m->cap = cap;
    m->used = m->len;
}

static void int_insert(IntMap *m, int64_t key, int64_t value) {
    size_t i = home(key, m->cap), gone = SIZE_MAX;
    while (m->slots[i].state != EMPTY) {
        if (m->slots[i].state == FULL && m->slots[i].key == key) {
            m->slots[i].value = value;
            return;
        }
        if (m->slots[i].state == GONE && gone == SIZE_MAX) gone = i;
        i = (i + 1) & (m->cap - 1);
    }
    if (gone != SIZE_MAX) i = gone;
    else m->used++;
    m->slots[i] = (Slot){key, value, FULL};
    m->len++;
    if (m->used * 2 > m->cap) int_grow(m);
}

static Slot *int_find(const IntMap *m, int64_t key) {
    size_t i = home(key, m->cap);
    while (m->slots[i].state != EMPTY) {
        if (m->slots[i].state == FULL && m->slots[i].key == key) return &m->slots[i];
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static void int_remove(IntMap *m, int64_t key) {
    Slot *s = int_find(m, key);
    if (s) {
        s->state = GONE;
        m->len--;
    }
}

typedef struct {
    const char *key;
    size_t len;
    int64_t value;
} StrSlot;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
    return h;
}

int main(int argc, char **argv) {
    char path[4096];
    snprintf(path, sizeof path, "%s/ints.bin", argv[1]);
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    int64_t *ints = malloc(size);
    if (fread(ints, 1, size, f) != size) return 1;
    fclose(f);
    size_t n = (size_t)atol(argv[2]);
    IntMap m = {calloc(16, sizeof(Slot)), 16, 0, 0};
    for (size_t i = 0; i < n; i++) int_insert(&m, ints[i], (int64_t)i);
    long long hits = 0, sum = 0;
    for (size_t i = 0; i < 2 * n; i++) {
        Slot *s = int_find(&m, ints[i]);
        if (s) {
            hits++;
            sum += s->value;
        }
    }
    printf("inserted %zu\nfound %lld %lld\n", m.len, hits, sum);
    for (size_t i = 0; i < n; i += 2) int_remove(&m, ints[i]);
    hits = 0;
    for (size_t i = 0; i < n; i++)
        if (int_find(&m, ints[i])) hits++;
    long long total = 0;
    for (size_t i = 0; i < m.cap; i++)
        if (m.slots[i].state == FULL) total += m.slots[i].value;
    printf("left %zu %lld %lld\n", m.len, hits, total);

    snprintf(path, sizeof path, "%s/strings.txt", argv[1]);
    f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    if (fread(text, 1, size, f) != size) return 1;
    fclose(f);
    text[size] = 0;
    size_t ns = 0, cap = 1024;
    char **words = malloc(cap * sizeof(char *));
    for (char *p = text; *p;) {
        char *nl = strchr(p, '\n');
        if (ns == cap) words = realloc(words, (cap *= 2) * sizeof(char *));
        words[ns++] = p;
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    size_t scap = 16, slen = 0;
    StrSlot *s = calloc(scap, sizeof(StrSlot));
    for (size_t i = 0; i < ns / 2; i++) {
        size_t len = strlen(words[i]);
        size_t at = fnv(words[i], len) & (scap - 1);
        while (s[at].key && (s[at].len != len || memcmp(s[at].key, words[i], len) != 0)) at = (at + 1) & (scap - 1);
        if (s[at].key) {
            s[at].value = (int64_t)i;
            continue;
        }
        char *copy = malloc(len + 1);
        memcpy(copy, words[i], len + 1);
        s[at] = (StrSlot){copy, len, (int64_t)i};
        if (++slen * 2 > scap) {
            size_t bigger = scap * 2;
            StrSlot *grown = calloc(bigger, sizeof(StrSlot));
            for (size_t j = 0; j < scap; j++) {
                if (!s[j].key) continue;
                size_t k = fnv(s[j].key, s[j].len) & (bigger - 1);
                while (grown[k].key) k = (k + 1) & (bigger - 1);
                grown[k] = s[j];
            }
            free(s);
            s = grown;
            scap = bigger;
        }
    }
    hits = sum = 0;
    for (size_t i = 0; i < ns; i++) {
        size_t len = strlen(words[i]);
        size_t at = fnv(words[i], len) & (scap - 1);
        while (s[at].key && (s[at].len != len || memcmp(s[at].key, words[i], len) != 0)) at = (at + 1) & (scap - 1);
        if (s[at].key) {
            hits++;
            sum += s[at].value;
        }
    }
    printf("strings %zu %lld %lld\n", slen, hits, sum);
    for (size_t j = 0; j < scap; j++) free((char *)s[j].key);
    free(s);
    free(words);
    free(text);
    free(m.slots);
    free(ints);
    return 0;
}
