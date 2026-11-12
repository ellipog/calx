// CSV report: a million rows of sales read with the language's CSV reader, some fields quoted with
// commas and doubled quotes inside, and summed by region and product and by month: the rows, the
// quantity and the revenue, in the order of the file. Calx uses std.csv, Rust the csv crate, Go
// encoding/csv and Python csv; C has none, so it reads the fields by hand, and keeps its 40 groups
// in an array it searches.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[128];
    long long rows, quantity;
    double revenue;
} Group;

// Reads one field at p into out, unquoting it, and returns where the next one starts: after the
// comma, or at the line's end.
static const char *field(const char *p, const char *end, char *out, size_t cap) {
    size_t n = 0;
    if (p < end && *p == '"') {
        p++;
        while (p < end) {
            if (*p == '"') {
                if (p + 1 < end && p[1] == '"') {
                    if (n + 1 < cap) out[n++] = '"';
                    p += 2;
                    continue;
                }
                p++;
                break;
            }
            if (n + 1 < cap) out[n++] = *p;
            p++;
        }
    }
    while (p < end && *p != ',' && *p != '\n' && *p != '\r') {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    if (p < end && *p == ',') p++;
    return p;
}

static int by_key(const void *a, const void *b) { return strcmp(((const Group *)a)->key, ((const Group *)b)->key); }

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size);
    if (fread(text, 1, size, f) != size) return 1;
    fclose(f);
    const char *p = memchr(text, '\n', size) + 1, *end = text + size;
    Group groups[64];
    size_t count = 0;
    double months[13] = {0};
    char date[32], region[64], product[64], quantity_text[32], price_text[32], key[128];
    while (p < end) {
        p = field(p, end, date, sizeof date);
        p = field(p, end, region, sizeof region);
        p = field(p, end, product, sizeof product);
        p = field(p, end, quantity_text, sizeof quantity_text);
        p = field(p, end, price_text, sizeof price_text);
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        long long quantity = atoll(quantity_text);
        double revenue = (double)quantity * strtod(price_text, NULL);
        snprintf(key, sizeof key, "%s|%s", region, product);
        Group *g = NULL;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(groups[i].key, key) == 0) {
                g = &groups[i];
                break;
            }
        }
        if (!g) {
            g = &groups[count++];
            strcpy(g->key, key);
            g->rows = g->quantity = 0;
            g->revenue = 0.0;
        }
        g->rows++;
        g->quantity += quantity;
        g->revenue += revenue;
        months[(date[5] - '0') * 10 + (date[6] - '0')] += revenue;
    }
    qsort(groups, count, sizeof(Group), by_key);
    for (size_t i = 0; i < count; i++)
        printf("%s|%lld|%lld|%.2f\n", groups[i].key, groups[i].rows, groups[i].quantity, groups[i].revenue);
    for (int m = 1; m <= 12; m++) printf("month %d %.2f\n", m, months[m]);
    free(text);
    return 0;
}
