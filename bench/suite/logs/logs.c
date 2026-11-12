// Log analysis: a million lines of a web server's access log taken apart by splitting, no regular
// expressions, and summed: requests by status and by method, requests and bytes by path, with
// query strings dropped and numeric ids folded, and by hour, and the distinct client addresses.
// C has no hash set, so the addresses go in one written here: open addressing, FNV-1a, doubled at
// half full. The few methods and paths are kept in arrays it searches.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **keys;
    size_t cap, len;
} Set;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
    return h;
}

static void set_add(Set *s, const char *key, size_t n) {
    size_t i = fnv(key, n) & (s->cap - 1);
    while (s->keys[i]) {
        if (strlen(s->keys[i]) == n && memcmp(s->keys[i], key, n) == 0) return;
        i = (i + 1) & (s->cap - 1);
    }
    char *k = malloc(n + 1);
    memcpy(k, key, n);
    k[n] = 0;
    s->keys[i] = k;
    if (++s->len * 2 > s->cap) {
        size_t cap = s->cap * 2;
        char **keys = calloc(cap, sizeof(char *));
        for (size_t j = 0; j < s->cap; j++) {
            if (!s->keys[j]) continue;
            size_t at = fnv(s->keys[j], strlen(s->keys[j])) & (cap - 1);
            while (keys[at]) at = (at + 1) & (cap - 1);
            keys[at] = s->keys[j];
        }
        free(s->keys);
        s->keys = keys;
        s->cap = cap;
    }
}

typedef struct {
    char name[64];
    long long n, bytes;
} Count;

static Count *find(Count *list, size_t *len, const char *name, size_t n) {
    for (size_t i = 0; i < *len; i++)
        if (strlen(list[i].name) == n && memcmp(list[i].name, name, n) == 0) return &list[i];
    Count *c = &list[(*len)++];
    memcpy(c->name, name, n);
    c->name[n] = 0;
    c->n = c->bytes = 0;
    return c;
}

static int by_name(const void *a, const void *b) { return strcmp(((const Count *)a)->name, ((const Count *)b)->name); }

static int by_count(const void *a, const void *b) {
    const Count *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int is_number(const char *s, size_t n) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    if (fread(text, 1, size, f) != size) return 1;
    fclose(f);
    text[size] = 0;
    long long lines = 0, statuses[600] = {0}, hours[24][2] = {{0}};
    Count methods[16], paths[64];
    size_t n_methods = 0, n_paths = 0;
    Set ips = {calloc(1024, sizeof(char *)), 1024, 0};
    char *p = text, *end = text + size;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line_end = nl ? nl : end;
        lines++;
        char *space = memchr(p, ' ', (size_t)(line_end - p));
        set_add(&ips, p, (size_t)(space - p));
        char *colon = memchr(space, ':', (size_t)(line_end - space));
        int hour = (colon[1] - '0') * 10 + (colon[2] - '0');
        char *request = memchr(space, '"', (size_t)(line_end - space)) + 1;
        char *close = memchr(request, '"', (size_t)(line_end - request));
        char *method_end = memchr(request, ' ', (size_t)(close - request));
        char *path = method_end + 1;
        char *path_end = memchr(path, ' ', (size_t)(close - path));
        char *q = memchr(path, '?', (size_t)(path_end - path));
        if (q) path_end = q;
        char folded[128];
        size_t path_len = (size_t)(path_end - path);
        memcpy(folded, path, path_len);
        char *slash = NULL;
        for (char *s = path_end; s > path; s--)
            if (s[-1] == '/') {
                slash = s - 1;
                break;
            }
        if (slash && is_number(slash + 1, (size_t)(path_end - slash - 1))) {
            path_len = (size_t)(slash + 1 - path);
            memcpy(folded + path_len, "{id}", 4);
            path_len += 4;
        }
        char *after = close + 1;
        while (*after == ' ') after++;
        int status = atoi(after);
        char *size_text = strchr(after, ' ') + 1;
        long long bytes = atoll(size_text);
        statuses[status]++;
        find(methods, &n_methods, request, (size_t)(method_end - request))->n++;
        Count *c = find(paths, &n_paths, folded, path_len);
        c->n++;
        c->bytes += bytes;
        hours[hour][0]++;
        hours[hour][1] += bytes;
        p = line_end + 1;
    }
    printf("lines %lld\nips %zu\n", lines, ips.len);
    for (int s = 0; s < 600; s++)
        if (statuses[s]) printf("status %d %lld\n", s, statuses[s]);
    qsort(methods, n_methods, sizeof(Count), by_name);
    for (size_t i = 0; i < n_methods; i++) printf("method %s %lld\n", methods[i].name, methods[i].n);
    qsort(paths, n_paths, sizeof(Count), by_count);
    for (size_t i = 0; i < n_paths; i++) printf("path %s %lld %lld\n", paths[i].name, paths[i].n, paths[i].bytes);
    for (int h = 0; h < 24; h++) printf("hour %02d %lld %lld\n", h, hours[h][0], hours[h][1]);
    for (size_t i = 0; i < ips.cap; i++) free(ips.keys[i]);
    free(ips.keys);
    free(text);
    return 0;
}
