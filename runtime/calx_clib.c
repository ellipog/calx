/* Shared libraries a program opens while it runs: C libraries for std.c's `CLibrary` (CLX-477),
 * and the loading that `Library` does for Calx ones, in calx_lib.c (D123). LoadLibraryExW on
 * Windows, which loads the DLLs a library needs from its own folder, then the system's, and
 * dlopen elsewhere, each symbol resolved when the library loads and none of them shared with
 * libraries loaded later.
 *
 * Linked into programs and static libraries that open one, and into every program that opens
 * Calx libraries. `cx_clib_open` leaves why it failed in a per-thread buffer, for
 * `cx_clib_error_len` and `cx_clib_error_copy`, which empties it. */

#include "calx_rt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

static void add(cx_String* s, const char* text) { cx_string_push_bytes(s, (const cx_u8*)text, strlen(text)); }

/* Whether `path` names a file by a path rather than a name the system looks for. */
static cx_bool has_slash(const cx_u8* path, cx_usize len) {
    for (cx_usize i = 0; i < len; i++) {
#ifdef _WIN32
        if (path[i] == '\\') return 1;
#endif
        if (path[i] == '/') return 1;
    }
    return 0;
}

#ifdef _WIN32
/* Windows' words for `code`, in English if it has them, without the full stop. */
static void system_words(DWORD code, cx_String* why) {
    wchar_t* msg = NULL;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                               code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (wchar_t*)&msg, 0, NULL);
    if (!len) {
        len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                             (wchar_t*)&msg, 0, NULL);
    }
    while (len && (msg[len - 1] == L'\n' || msg[len - 1] == L'\r' || msg[len - 1] == L' ' || msg[len - 1] == L'.')) len--;
    if (len) {
        int m = WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, NULL, 0, NULL, NULL);
        cx_string_reserve(why, (cx_usize)m);
        WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, (char*)why->ptr + why->len, m, NULL, NULL);
        why->len += (cx_usize)m;
    } else {
        add(why, "Windows can't load it");
    }
    if (msg) LocalFree(msg);
}

void* cx_dl_load(const cx_u8* path, cx_usize len, cx_bool search, cx_String* why) {
    cx_i32 n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path, (int)len, NULL, 0);
    if ((n <= 0 && len) || memchr(path, 0, len)) {
        add(why, "isn't a path Windows can open");
        return NULL;
    }
    wchar_t* w = (wchar_t*)cx_alloc(((cx_usize)n + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, (const char*)path, (int)len, w, n);
    w[n] = 0;
    HMODULE h = NULL;
    DWORD code = ERROR_BAD_PATHNAME;
    if (search && !has_slash(path, len)) {
        /* A name, such as kernel32.dll, from the program's folder or the system's, as ctypes
         * finds one, and never from the current folder or PATH. */
        h = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        code = h ? 0 : GetLastError();
    } else {
        /* A path, from the current folder as on Linux, rather than a name Windows looks for in its
         * own folders. The DLLs it needs come from its folder, then the system's. */
        DWORD full_len = GetFullPathNameW(w, 0, NULL, NULL);
        wchar_t* full = full_len ? (wchar_t*)cx_alloc((cx_usize)full_len * sizeof(wchar_t)) : NULL;
        if (full && GetFullPathNameW(w, full_len, full, NULL)) {
            h = LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            code = h ? 0 : GetLastError();
        }
        if (full) cx_free(full, (cx_usize)full_len * sizeof(wchar_t));
    }
    cx_free(w, ((cx_usize)n + 1) * sizeof(wchar_t));
    if (!h) system_words(code, why);
    return h;
}

void* cx_dl_symbol(void* h, const char* name) { return (void*)GetProcAddress((HMODULE)h, name); }
void cx_dl_unload(void* h) { FreeLibrary((HMODULE)h); }
#else
void* cx_dl_load(const cx_u8* path, cx_usize len, cx_bool search, cx_String* why) {
    if (memchr(path, 0, len)) {
        add(why, "a path can't hold a NUL");
        return NULL;
    }
    /* A path without a slash would be looked for on the system's library path, not here, unless
     * that's what's asked for. */
    cx_bool name = !has_slash(path, len);
    cx_bool here = name && !search;
    cx_usize size = len + 1 + (here ? 2 : 0);
    char* p = (char*)cx_alloc(size);
    if (here) memcpy(p, "./", 2);
    memcpy(p + (here ? 2 : 0), path, len);
    p[size - 1] = 0;
    void* h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        /* dlerror starts with the path, which the message already has. */
        const char* e = dlerror();
        const char* colon = e ? strstr(e, ": ") : NULL;
        add(why, colon ? colon + 2 : e ? e : "it can't be loaded");
#ifdef CX_IOS
        /* A file that's there but won't load is, on an iPhone, most often one the app didn't ship
         * with: iOS runs only code signed into the app. */
        struct stat st;
        if (!name && stat(p, &st) == 0) {
            add(why, "; iOS opens only libraries signed into the app's bundle, so never one it downloaded");
        }
#endif
    }
    cx_free(p, size);
    return h;
}

void* cx_dl_symbol(void* h, const char* name) { return dlsym(h, name); }
void cx_dl_unload(void* h) { dlclose(h); }
#endif

/* Why the last `cx_clib_open` on this thread failed. malloc's, since it can outlive what the
 * allocator's leak check counts. */
static _Thread_local cx_u8* err_buf;
static _Thread_local size_t err_len;

static void err_set(const cx_String* s) {
    free(err_buf);
    err_buf = NULL;
    err_len = 0;
    if (!s->len) return;
    err_buf = (cx_u8*)malloc(s->len);
    if (!err_buf) return;
    memcpy(err_buf, s->ptr, s->len);
    err_len = s->len;
}

void* cx_clib_open(const cx_u8* path, cx_usize len) {
    cx_String why = {0};
    void* h = cx_dl_load(path, len, 1, &why);
    if (!h) {
        cx_String e = {0};
        add(&e, "can't open ");
        cx_string_push_bytes(&e, path, len);
        add(&e, ": ");
        cx_string_push_bytes(&e, why.ptr, why.len);
        err_set(&e);
        cx_drop_String(&e);
    }
    cx_drop_String(&why);
    return h;
}

cx_usize cx_clib_error_len(void) { return err_len; }

void cx_clib_error_copy(cx_u8* out) {
    if (err_len) memcpy(out, err_buf, err_len);
    free(err_buf);
    err_buf = NULL;
    err_len = 0;
}

void* cx_clib_symbol(void* h, const cx_u8* name, cx_usize len) {
    if (!len || memchr(name, 0, len)) return NULL;
    char stack[256];
    char* n = len < sizeof stack ? stack : (char*)cx_alloc(len + 1);
    memcpy(n, name, len);
    n[len] = 0;
    void* p = cx_dl_symbol(h, n);
    if (n != stack) cx_free(n, len + 1);
    return p;
}

void cx_clib_close(void* h) { cx_dl_unload(h); }
