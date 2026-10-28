/* The Calx runtime's files and directories, behind std.fs.
 *
 * Paths come from Calx as UTF-8 with a length. On Windows they're converted to UTF-16 for the
 * wide API, so any name works; on Linux they're passed as they are. Each call that fails records
 * why for `cx_fs_error_kind` and `cx_fs_error_code`, per thread.
 *
 * A file is opened, read or written, and closed through a handle, a HANDLE or a descriptor as an
 * i64, so std.fs reads a whole file in a few large reads without a size the file could outgrow.
 * `cx_fs_open`'s `write` is 0 to read, 1 to replace what the file holds, and 2 to add to its end.
 * Results with no fixed size, a directory's names or a new temporary directory's path, go to a
 * per-thread buffer that std.fs copies out.
 *
 * This file is compiled into a program only when it uses std.fs. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calx_rt.h"

CX_COLD void cx_panic_text(const char* at, const char* message);

enum { FS_NOT_FOUND = 1, FS_DENIED = 2, FS_EXISTS = 3, FS_BAD_PATH = 4, FS_IS_DIR = 5, FS_NOT_EMPTY = 6, FS_OTHER = 7 };
enum { KIND_NONE = 0, KIND_FILE = 1, KIND_DIR = 2, KIND_LINK = 3, KIND_OTHER = 4, KIND_ERROR = 5 };

static _Thread_local cx_u32 fs_err_kind;
static _Thread_local cx_i32 fs_err_code;

/* The per-thread result buffer. */
static _Thread_local char* fs_buf;
static _Thread_local size_t fs_len, fs_cap;

static CX_COLD void fs_out_of_memory(void) { cx_panic_text("<memory>", "out of memory"); }

static void fs_fail(cx_u32 kind, cx_i32 code) {
    fs_err_kind = kind;
    fs_err_code = code;
}

static void buf_add(const char* s, size_t n) {
    if (fs_len + n > fs_cap) {
        size_t c = fs_cap ? fs_cap : 256;
        while (c < fs_len + n) c *= 2;
        char* p = (char*)realloc(fs_buf, c);
        if (!p) fs_out_of_memory();
        fs_buf = p;
        fs_cap = c;
    }
    memcpy(fs_buf + fs_len, s, n);
    fs_len += n;
}

/* A path with a NUL in it would name a different file than it says. */
static int path_ok(const cx_u8* p, size_t n) {
    if (memchr(p, 0, n)) {
        fs_fail(FS_BAD_PATH, 0);
        return 0;
    }
    return 1;
}

#ifdef _WIN32

static void fail_win(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
        fs_fail(FS_NOT_FOUND, (cx_i32)e);
        break;
    case ERROR_ACCESS_DENIED:
        fs_fail(FS_DENIED, (cx_i32)e);
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        fs_fail(FS_EXISTS, (cx_i32)e);
        break;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
        fs_fail(FS_BAD_PATH, (cx_i32)e);
        break;
    case ERROR_DIR_NOT_EMPTY:
        fs_fail(FS_NOT_EMPTY, (cx_i32)e);
        break;
    default:
        fs_fail(FS_OTHER, (cx_i32)e);
        break;
    }
}

/* The path as UTF-16, NUL-terminated, which the caller frees; NULL if it can't be. */
static wchar_t* wide(const cx_u8* p, size_t n) {
    if (!path_ok(p, n)) return NULL;
    if (n > 0x7FFFFFFF) {
        fs_fail(FS_BAD_PATH, 0);
        return NULL;
    }
    int units = n ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, NULL, 0) : 0;
    if (n && units == 0) {
        fs_fail(FS_BAD_PATH, (cx_i32)GetLastError());
        return NULL;
    }
    wchar_t* w = (wchar_t*)malloc(((size_t)units + 1) * sizeof(wchar_t));
    if (!w) fs_out_of_memory();
    if (units) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, w, units);
    w[units] = 0;
    return w;
}

/* Adds UTF-16 text to the result buffer as UTF-8. An unpaired surrogate becomes U+FFFD. */
static void buf_add_wide(const wchar_t* w, int units) {
    if (units == 0) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, units, NULL, 0, NULL, NULL);
    if (bytes <= 0) return;
    char* s = (char*)malloc((size_t)bytes);
    if (!s) fs_out_of_memory();
    WideCharToMultiByte(CP_UTF8, 0, w, units, s, bytes, NULL, NULL);
    buf_add(s, (size_t)bytes);
    free(s);
}

cx_i64 cx_fs_open(const cx_u8* path, size_t len, cx_u32 write) {
    wchar_t* w = wide(path, len);
    if (!w) return -1;
    HANDLE h = write == 2 ? CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
             : write ? CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                     : CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        DWORD attrs = GetFileAttributesW(w);
        if (e == ERROR_ACCESS_DENIED && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            fs_fail(FS_IS_DIR, (cx_i32)e);
        } else {
            fail_win(e);
        }
        free(w);
        return -1;
    }
    free(w);
    return (cx_i64)(intptr_t)h;
}

cx_i64 cx_fs_size(cx_i64 h) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)(intptr_t)h, &size)) return -1;
    return (cx_i64)size.QuadPart;
}

/* Moves the next read to `at` bytes from the start. */
cx_i32 cx_fs_seek(cx_i64 h, cx_u64 at) {
    LARGE_INTEGER to;
    to.QuadPart = (LONGLONG)at;
    if (!SetFilePointerEx((HANDLE)(intptr_t)h, to, NULL, FILE_BEGIN)) {
        fail_win(GetLastError());
        return 0;
    }
    return 1;
}

cx_i64 cx_fs_read(cx_i64 h, cx_u8* to, size_t n) {
    DWORD want = n > 0x40000000 ? 0x40000000 : (DWORD)n;
    DWORD got = 0;
    if (!ReadFile((HANDLE)(intptr_t)h, to, want, &got, NULL)) {
        fail_win(GetLastError());
        return -1;
    }
    return (cx_i64)got;
}

cx_i32 cx_fs_write(cx_i64 h, const cx_u8* from, size_t n) {
    while (n > 0) {
        DWORD want = n > 0x40000000 ? 0x40000000 : (DWORD)n;
        DWORD put = 0;
        if (!WriteFile((HANDLE)(intptr_t)h, from, want, &put, NULL)) {
            fail_win(GetLastError());
            return 0;
        }
        from += put;
        n -= put;
    }
    return 1;
}

cx_i32 cx_fs_close(cx_i64 h) {
    if (!CloseHandle((HANDLE)(intptr_t)h)) {
        fail_win(GetLastError());
        return 0;
    }
    return 1;
}

cx_i32 cx_fs_rename(const cx_u8* from, size_t from_len, const cx_u8* to, size_t to_len) {
    wchar_t* a = wide(from, from_len);
    if (!a) return 0;
    wchar_t* b = wide(to, to_len);
    if (!b) {
        free(a);
        return 0;
    }
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    if (!ok) fail_win(GetLastError());
    free(a);
    free(b);
    return ok ? 1 : 0;
}

cx_i32 cx_fs_remove_file(const cx_u8* path, size_t len) {
    wchar_t* w = wide(path, len);
    if (!w) return 0;
    BOOL ok = DeleteFileW(w);
    if (!ok) {
        DWORD e = GetLastError();
        DWORD attrs = GetFileAttributesW(w);
        if (e == ERROR_ACCESS_DENIED && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            fs_fail(FS_IS_DIR, (cx_i32)e);
        } else {
            fail_win(e);
        }
    }
    free(w);
    return ok ? 1 : 0;
}

cx_i32 cx_fs_remove_dir(const cx_u8* path, size_t len) {
    wchar_t* w = wide(path, len);
    if (!w) return 0;
    BOOL ok = RemoveDirectoryW(w);
    if (!ok) fail_win(GetLastError());
    free(w);
    return ok ? 1 : 0;
}

cx_i32 cx_fs_make_dir(const cx_u8* path, size_t len) {
    wchar_t* w = wide(path, len);
    if (!w) return 0;
    BOOL ok = CreateDirectoryW(w, NULL);
    if (!ok) fail_win(GetLastError());
    free(w);
    return ok ? 1 : 0;
}

/* Whether a reparse point is a link: a symbolic link or a junction. Others, such as cloud files,
 * are what they appear to be. */
static int is_link(const wchar_t* w) {
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileW(w, &d);
    if (f == INVALID_HANDLE_VALUE) return 0;
    FindClose(f);
    return d.dwReserved0 == IO_REPARSE_TAG_SYMLINK || d.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}

cx_u32 cx_fs_kind(const cx_u8* path, size_t len, cx_u32 follow) {
    wchar_t* w = wide(path, len);
    if (!w) return KIND_ERROR;
    DWORD attrs = GetFileAttributesW(w);
    cx_u32 kind;
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME || e == ERROR_BAD_NETPATH) {
            kind = KIND_NONE;
        } else {
            fail_win(e);
            kind = KIND_ERROR;
        }
    } else if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && is_link(w)) {
        if (!follow) {
            kind = KIND_LINK;
        } else {
            /* What the link leads to, if anything. */
            HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, NULL);
            BY_HANDLE_FILE_INFORMATION info;
            if (h == INVALID_HANDLE_VALUE) {
                kind = KIND_NONE;
            } else {
                kind = !GetFileInformationByHandle(h, &info)                     ? KIND_NONE
                       : (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? KIND_DIR
                                                                            : KIND_FILE;
                CloseHandle(h);
            }
        }
    } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        kind = KIND_DIR;
    } else if (attrs & FILE_ATTRIBUTE_DEVICE) {
        kind = KIND_OTHER;
    } else {
        kind = KIND_FILE;
    }
    free(w);
    return kind;
}

/* The names in a directory, each followed by a NUL, in the result buffer. */
cx_i32 cx_fs_list(const cx_u8* path, size_t len) {
    fs_len = 0;
    if (!path_ok(path, len)) return 0;
    /* `path\*`, with no separator doubled. */
    size_t n = len;
    cx_u8* pattern = (cx_u8*)malloc(n + 3);
    if (!pattern) fs_out_of_memory();
    memcpy(pattern, path, n);
    if (n > 0 && pattern[n - 1] != '/' && pattern[n - 1] != '\\') pattern[n++] = '\\';
    pattern[n++] = '*';
    wchar_t* w = wide(pattern, n);
    free(pattern);
    if (!w) return 0;
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileExW(w, FindExInfoBasic, &d, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(w);
        if (e == ERROR_FILE_NOT_FOUND) return 1; /* an empty drive root */
        if (e == ERROR_DIRECTORY) e = ERROR_PATH_NOT_FOUND;
        fail_win(e);
        return 0;
    }
    free(w);
    do {
        const wchar_t* name = d.cFileName;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        buf_add_wide(name, (int)wcslen(name));
        buf_add("", 1);
    } while (FindNextFileW(f, &d));
    DWORD e = GetLastError();
    FindClose(f);
    if (e != ERROR_NO_MORE_FILES) {
        fail_win(e);
        return 0;
    }
    return 1;
}

/* A new, empty directory in the system's temporary folder, whose path goes in the result buffer. */
cx_i32 cx_fs_temp_dir(void) {
    static volatile LONG counter;
    wchar_t base[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, base);
    if (n == 0 || n > MAX_PATH) {
        fail_win(n == 0 ? GetLastError() : ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    for (int attempt = 0; attempt < 100; attempt++) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        wchar_t name[MAX_PATH + 64];
        swprintf(name, MAX_PATH + 64, L"%lscalx-%lx-%lx-%llx", base, (unsigned long)GetCurrentProcessId(),
                 (unsigned long)InterlockedIncrement(&counter), (unsigned long long)t.QuadPart);
        if (CreateDirectoryW(name, NULL)) {
            fs_len = 0;
            buf_add_wide(name, (int)wcslen(name));
            return 1;
        }
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            fail_win(e);
            return 0;
        }
    }
    fail_win(ERROR_ALREADY_EXISTS);
    return 0;
}

/* The path with every link followed, as the system names what it leads to, in the result
 * buffer: `C:\...`, or `\\server\share\...` on a network, without the `\\?\` the system adds. */
cx_i32 cx_fs_real_path(const cx_u8* path, size_t len) {
    fs_len = 0;
    wchar_t* w = wide(path, len);
    if (!w) return 0;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        fail_win(GetLastError());
        return 0;
    }
    wchar_t small[MAX_PATH + 1];
    wchar_t* name = small;
    DWORD n = GetFinalPathNameByHandleW(h, small, MAX_PATH + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n > MAX_PATH) {
        name = (wchar_t*)malloc(((size_t)n + 1) * sizeof(wchar_t));
        if (!name) fs_out_of_memory();
        n = GetFinalPathNameByHandleW(h, name, n + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    DWORD e = GetLastError();
    CloseHandle(h);
    if (n == 0) {
        if (name != small) free(name);
        fail_win(e);
        return 0;
    }
    const wchar_t* s = name;
    if (n >= 8 && wcsncmp(s, L"\\\\?\\UNC\\", 8) == 0) {
        buf_add("\\\\", 2);
        s += 8;
        n -= 8;
    } else if (n >= 4 && wcsncmp(s, L"\\\\?\\", 4) == 0) {
        s += 4;
        n -= 4;
    }
    buf_add_wide(s, (int)n);
    if (name != small) free(name);
    return 1;
}

#else

static void fail_errno(int e) {
    switch (e) {
    case ENOENT:
        fs_fail(FS_NOT_FOUND, e);
        break;
    case EACCES:
    case EPERM:
    case EROFS:
        fs_fail(FS_DENIED, e);
        break;
    case EEXIST:
        fs_fail(FS_EXISTS, e);
        break;
    case EISDIR:
        fs_fail(FS_IS_DIR, e);
        break;
    case ENOTEMPTY:
        fs_fail(FS_NOT_EMPTY, e);
        break;
    default:
        fs_fail(FS_OTHER, e);
        break;
    }
}

/* The path, NUL-terminated, which the caller frees; NULL if it can't be. */
static char* cpath(const cx_u8* p, size_t n) {
    if (!path_ok(p, n)) return NULL;
    char* s = (char*)malloc(n + 1);
    if (!s) fs_out_of_memory();
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

cx_i64 cx_fs_open(const cx_u8* path, size_t len, cx_u32 write) {
    char* s = cpath(path, len);
    if (!s) return -1;
    int fd = write == 2 ? open(s, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666)
             : write ? open(s, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666) : open(s, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fail_errno(errno);
        free(s);
        return -1;
    }
    free(s);
    if (!write) {
        /* Opening a directory works on Linux; reading it is the error, so say so now. */
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            fs_fail(FS_IS_DIR, EISDIR);
            return -1;
        }
    }
    return fd;
}

cx_i64 cx_fs_size(cx_i64 h) {
    struct stat st;
    if (fstat((int)h, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (cx_i64)st.st_size;
}

/* Moves the next read to `at` bytes from the start. */
cx_i32 cx_fs_seek(cx_i64 h, cx_u64 at) {
    if (lseek((int)h, (off_t)at, SEEK_SET) < 0) {
        fail_errno(errno);
        return 0;
    }
    return 1;
}

cx_i64 cx_fs_read(cx_i64 h, cx_u8* to, size_t n) {
    for (;;) {
        ssize_t got = read((int)h, to, n > 0x40000000 ? 0x40000000 : n);
        if (got >= 0) return (cx_i64)got;
        if (errno != EINTR) {
            fail_errno(errno);
            return -1;
        }
    }
}

cx_i32 cx_fs_write(cx_i64 h, const cx_u8* from, size_t n) {
    while (n > 0) {
        ssize_t put = write((int)h, from, n > 0x40000000 ? 0x40000000 : n);
        if (put < 0) {
            if (errno == EINTR) continue;
            fail_errno(errno);
            return 0;
        }
        from += put;
        n -= (size_t)put;
    }
    return 1;
}

cx_i32 cx_fs_close(cx_i64 h) {
    if (close((int)h) != 0 && errno != EINTR) {
        fail_errno(errno);
        return 0;
    }
    return 1;
}

cx_i32 cx_fs_rename(const cx_u8* from, size_t from_len, const cx_u8* to, size_t to_len) {
    char* a = cpath(from, from_len);
    if (!a) return 0;
    char* b = cpath(to, to_len);
    if (!b) {
        free(a);
        return 0;
    }
    int ok = rename(a, b) == 0;
    if (!ok) fail_errno(errno);
    free(a);
    free(b);
    return ok;
}

cx_i32 cx_fs_remove_file(const cx_u8* path, size_t len) {
    char* s = cpath(path, len);
    if (!s) return 0;
    int ok = unlink(s) == 0;
    if (!ok) fail_errno(errno);
    free(s);
    return ok;
}

cx_i32 cx_fs_remove_dir(const cx_u8* path, size_t len) {
    char* s = cpath(path, len);
    if (!s) return 0;
    int ok = rmdir(s) == 0;
    if (!ok) fail_errno(errno == EEXIST ? ENOTEMPTY : errno);
    free(s);
    return ok;
}

cx_i32 cx_fs_make_dir(const cx_u8* path, size_t len) {
    char* s = cpath(path, len);
    if (!s) return 0;
    int ok = mkdir(s, 0777) == 0;
    if (!ok) fail_errno(errno);
    free(s);
    return ok;
}

cx_u32 cx_fs_kind(const cx_u8* path, size_t len, cx_u32 follow) {
    char* s = cpath(path, len);
    if (!s) return KIND_ERROR;
    struct stat st;
    int r = follow ? stat(s, &st) : lstat(s, &st);
    int e = errno;
    free(s);
    if (r != 0) {
        if (e == ENOENT || e == ENOTDIR) return KIND_NONE;
        fail_errno(e);
        return KIND_ERROR;
    }
    if (S_ISREG(st.st_mode)) return KIND_FILE;
    if (S_ISDIR(st.st_mode)) return KIND_DIR;
    if (S_ISLNK(st.st_mode)) return KIND_LINK;
    return KIND_OTHER;
}

/* Adds a name to the result buffer as UTF-8, with U+FFFD for each maximal part of it that isn't
 * UTF-8, as the Unicode standard recommends. */
static void buf_add_name(const unsigned char* s, size_t n) {
    static const char replacement[] = "\xEF\xBF\xBD";
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t need;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c < 0x80) {
            buf_add((const char*)s + i, 1);
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            buf_add(replacement, 3);
            i++;
            continue;
        }
        size_t j = 1;
        while (j <= need && i + j < n) {
            unsigned char d = s[i + j];
            if (d < lo || d > hi) break;
            lo = 0x80;
            hi = 0xBF;
            j++;
        }
        if (j == need + 1) {
            buf_add((const char*)s + i, j);
        } else {
            buf_add(replacement, 3);
        }
        i += j;
    }
}

cx_i32 cx_fs_list(const cx_u8* path, size_t len) {
    fs_len = 0;
    char* s = cpath(path, len);
    if (!s) return 0;
    DIR* d = opendir(s);
    free(s);
    if (!d) {
        fail_errno(errno == ENOTDIR ? ENOENT : errno);
        return 0;
    }
    for (;;) {
        errno = 0;
        struct dirent* e = readdir(d);
        if (!e) break;
        const char* name = e->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        buf_add_name((const unsigned char*)name, strlen(name));
        buf_add("", 1);
    }
    int err = errno;
    closedir(d);
    if (err) {
        fail_errno(err);
        return 0;
    }
    return 1;
}

cx_i32 cx_fs_temp_dir(void) {
    const char* base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/') n--;
    char* t = (char*)malloc(n + 13);
    if (!t) fs_out_of_memory();
    memcpy(t, base, n);
    memcpy(t + n, "/calx-XXXXXX", 13);
    if (!mkdtemp(t)) {
        fail_errno(errno);
        free(t);
        return 0;
    }
    fs_len = 0;
    buf_add(t, strlen(t));
    free(t);
    return 1;
}

/* The absolute path with every link, `.` and `..` resolved, in the result buffer. */
cx_i32 cx_fs_real_path(const cx_u8* path, size_t len) {
    fs_len = 0;
    char* s = cpath(path, len);
    if (!s) return 0;
    char* r = realpath(s, NULL);
    int e = errno;
    free(s);
    if (!r) {
        fail_errno(e == ENOTDIR ? ENOENT : e);
        return 0;
    }
    buf_add_name((const unsigned char*)r, strlen(r));
    free(r);
    return 1;
}

#endif

size_t cx_fs_result_len(void) { return fs_len; }
void cx_fs_result_copy(cx_u8* to) {
    if (fs_len) memcpy(to, fs_buf, fs_len);
}
cx_u32 cx_fs_error_kind(void) { return fs_err_kind; }
cx_i32 cx_fs_error_code(void) { return fs_err_code; }
