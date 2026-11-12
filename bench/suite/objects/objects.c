// Many small objects: 20 rounds of 200,000 records, each with an id, a name made with formatting
// and a list of 1 to 8 numbers, three allocations a record. Each round's records are summed, one
// in 16 kept for the rest of the run, and the others freed.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t id;
    char *name;
    int64_t *values;
    size_t len;
} Record;

int main(int argc, char **argv) {
    int64_t rounds = atoll(argv[1]), per = atoll(argv[2]);
    size_t kept_len = 0, kept_cap = 1024;
    Record **kept = malloc(kept_cap * sizeof(Record *));
    int64_t total = 0;
    for (int64_t round = 0; round < rounds; round++) {
        size_t n = 0, cap = 16;
        Record **batch = malloc(cap * sizeof(Record *));
        for (int64_t i = 0; i < per; i++) {
            int64_t id = round * per + i;
            Record *r = malloc(sizeof(Record));
            r->id = id;
            char name[32];
            int len = snprintf(name, sizeof name, "item-%lld", (long long)id);
            r->name = malloc((size_t)len + 1);
            memcpy(r->name, name, (size_t)len + 1);
            r->len = (size_t)(1 + id % 8);
            r->values = malloc(r->len * sizeof(int64_t));
            for (size_t k = 0; k < r->len; k++) r->values[k] = id * (int64_t)(k + 1);
            if (n == cap) batch = realloc(batch, (cap *= 2) * sizeof(Record *));
            batch[n++] = r;
        }
        for (size_t j = 0; j < n; j++) {
            for (size_t k = 0; k < batch[j]->len; k++) total += batch[j]->values[k];
            total += (int64_t)strlen(batch[j]->name);
        }
        for (size_t j = 0; j < n; j++) {
            Record *r = batch[j];
            if (r->id % 16 == 0) {
                if (kept_len == kept_cap) kept = realloc(kept, (kept_cap *= 2) * sizeof(Record *));
                kept[kept_len++] = r;
            } else {
                free(r->name);
                free(r->values);
                free(r);
            }
        }
        free(batch);
    }
    size_t names = 0;
    int64_t values = 0;
    for (size_t j = 0; j < kept_len; j++) {
        names += strlen(kept[j]->name);
        for (size_t k = 0; k < kept[j]->len; k++) values += kept[j]->values[k];
    }
    printf("total %lld\nkept %zu %zu %lld\n", (long long)total, kept_len, names, (long long)values);
    for (size_t j = 0; j < kept_len; j++) {
        free(kept[j]->name);
        free(kept[j]->values);
        free(kept[j]);
    }
    free(kept);
    return 0;
}
