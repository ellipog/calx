// Many files: 2,000 files of 1 to 16 KB written into 20 folders, read back and their bytes added
// up, then removed with their folders, through the language's own file functions: stdio's, with
// the system's mkdir and rmdir, which C's standard library doesn't have.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0755)
#define remove_dir(p) rmdir(p)
#endif

static void check(int ok, const char *what) {
    if (!ok) {
        perror(what);
        exit(1);
    }
}

int main(int argc, char **argv) {
    char root[4096], path[4200];
    snprintf(root, sizeof root, "%s/run", argv[1]);
    int files = atoi(argv[2]);
    static unsigned char pattern[16384 + 251], data[16384];
    for (int j = 0; j < 16384 + 251; j++) pattern[j] = (unsigned char)(j % 251);
    check(make_dir(root) == 0, root);
    for (int d = 0; d < 20; d++) {
        snprintf(path, sizeof path, "%s/d%d", root, d);
        check(make_dir(path) == 0, path);
    }
    size_t written = 0;
    for (int i = 0; i < files; i++) {
        size_t size = 1024 + ((size_t)i * 7919) % 15000;
        snprintf(path, sizeof path, "%s/d%d/f%d.bin", root, i % 20, i);
        FILE *f = fopen(path, "wb");
        check(f != NULL, path);
        check(fwrite(pattern + i % 251, 1, size, f) == size, path);
        fclose(f);
        written += size;
    }
    uint64_t sum = 0;
    size_t read = 0;
    for (int i = 0; i < files; i++) {
        snprintf(path, sizeof path, "%s/d%d/f%d.bin", root, i % 20, i);
        FILE *f = fopen(path, "rb");
        check(f != NULL, path);
        size_t n = fread(data, 1, sizeof data, f);
        fclose(f);
        read += n;
        for (size_t k = 0; k < n; k++) sum += data[k];
    }
    for (int i = 0; i < files; i++) {
        snprintf(path, sizeof path, "%s/d%d/f%d.bin", root, i % 20, i);
        check(remove(path) == 0, path);
    }
    for (int d = 0; d < 20; d++) {
        snprintf(path, sizeof path, "%s/d%d", root, d);
        check(remove_dir(path) == 0, path);
    }
    check(remove_dir(root) == 0, root);
    printf("files %d\nwritten %zu\nread %zu\nsum %llu\n", files, written, read, (unsigned long long)sum);
    return 0;
}
