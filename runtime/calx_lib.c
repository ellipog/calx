/* Libraries a program opens while it runs, for `Library` (D123). Linked only into programs that
 * open them, which then have the whole runtime, so any library finds every function it needs.
 *
 * A library has no runtime of its own: each runtime function in it is a pointer, which opening it
 * fills in from `cx_runtime_fns`, the program's own in calx_rt.h's order. So the two share one
 * allocator, one output and one way to panic, and memory one allocates the other can free. Both
 * hold the hash of the compiler and the runtime header they were built with, and a library built
 * with another is refused before anything in it runs. */

#include "calx_rt.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Written by the compiler for each program that opens libraries. */
extern void* const cx_runtime_fns[];
extern const cx_usize cx_runtime_count;
extern const cx_u64 cx_runtime_abi;

typedef struct {
    void* handle;
    const cx_lib_export* exports;
    cx_usize count;
    cx_String path;
} lib;

static void add(cx_String* s, const char* text) { cx_string_push_bytes(s, (const cx_u8*)text, strlen(text)); }
static void add_str(cx_String* s, cx_str text) { cx_string_push_bytes(s, text.ptr, text.len); }

#ifdef _WIN32
static void* load(const cx_String* path, cx_String* error) {
    cx_i32 n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path->ptr, (int)path->len, NULL, 0);
    if (n <= 0 && path->len) {
        add(error, "isn't a path Windows can open");
        return NULL;
    }
    wchar_t* w = (wchar_t*)cx_alloc(((cx_usize)n + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, (const char*)path->ptr, (int)path->len, w, n);
    w[n] = 0;
    /* A path, from the current folder as on Linux, rather than a name Windows looks for in its
     * own folders. The DLLs it needs come from its folder, then the system's. */
    DWORD full_len = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t* full = full_len ? (wchar_t*)cx_alloc((cx_usize)full_len * sizeof(wchar_t)) : NULL;
    HMODULE h = NULL;
    DWORD code = ERROR_BAD_PATHNAME;
    if (full && GetFullPathNameW(w, full_len, full, NULL)) {
        h = LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        code = h ? 0 : GetLastError();
    }
    if (full) cx_free(full, (cx_usize)full_len * sizeof(wchar_t));
    cx_free(w, ((cx_usize)n + 1) * sizeof(wchar_t));
    if (!h) {
        wchar_t* msg = NULL;
        DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   NULL, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (wchar_t*)&msg, 0, NULL);
        if (!len) len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       NULL, code, 0, (wchar_t*)&msg, 0, NULL);
        while (len && (msg[len - 1] == L'\n' || msg[len - 1] == L'\r' || msg[len - 1] == L' ' || msg[len - 1] == L'.')) len--;
        if (len) {
            int m = WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, NULL, 0, NULL, NULL);
            cx_string_reserve(error, (cx_usize)m);
            WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, (char*)error->ptr + error->len, m, NULL, NULL);
            error->len += (cx_usize)m;
        } else {
            add(error, "Windows can't load it");
        }
        if (msg) LocalFree(msg);
    }
    return h;
}
static void* symbol(void* h, const char* name) { return (void*)GetProcAddress((HMODULE)h, name); }
static void unload(void* h) { FreeLibrary((HMODULE)h); }
#else
static void* load(const cx_String* path, cx_String* error) {
    char* p = (char*)cx_alloc(path->len + 1);
    memcpy(p, path->ptr, path->len);
    p[path->len] = 0;
    /* A path without a slash would be looked for on the system's library path, not here. */
    void* h = NULL;
    if (memchr(p, '/', path->len)) {
        h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    } else {
        cx_free(p, path->len + 1);
        p = (char*)cx_alloc(path->len + 3);
        memcpy(p, "./", 2);
        memcpy(p + 2, path->ptr, path->len);
        p[path->len + 2] = 0;
        h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        cx_free(p, path->len + 3);
        p = NULL;
    }
    if (p) cx_free(p, path->len + 1);
    if (!h) {
        /* dlerror starts with the path, which the message already has. */
        const char* e = dlerror();
        const char* colon = e ? strstr(e, ": ") : NULL;
        add(error, colon ? colon + 2 : e ? e : "it can't be loaded");
    }
    return h;
}
static void* symbol(void* h, const char* name) { return dlsym(h, name); }
static void unload(void* h) { dlclose(h); }
#endif

cx_bool cx_lib_open(cx_str path, cx_Library* out, cx_String* error) {
    out->p = NULL;
    *error = (cx_String){0};
    cx_String p = cx_string_from(path);
    cx_String why = {0};
    void* h = load(&p, &why);
    if (!h) {
        add(error, "can't open ");
        add_str(error, path);
        add(error, ": ");
        cx_string_push_bytes(error, why.ptr, why.len);
        cx_drop_String(&why);
        cx_drop_String(&p);
        return 0;
    }
    const cx_lib_info* info = (const cx_lib_info*)symbol(h, "cx_library");
    const char* wrong = NULL;
    if (!info) wrong = " isn't a Calx library: it has no `cx_library`";
    else if (info->abi != cx_runtime_abi || info->fns != cx_runtime_count) {
        wrong = " was built by another build of Calx than this program was; build both with the same `calx`";
    }
    if (wrong) {
        unload(h);
        add_str(error, path);
        add(error, wrong);
        cx_drop_String(&p);
        return 0;
    }
    info->attach(cx_runtime_fns);
    lib* l = (lib*)cx_alloc(sizeof(lib));
    l->handle = h;
    l->exports = info->exports(&l->count);
    l->path = p;
    out->p = l;
    return 1;
}

static const cx_lib_export* lookup(const lib* l, cx_str name) {
    for (cx_usize i = 0; i < l->count; i++) {
        const cx_lib_export* e = &l->exports[i];
        if (strlen(e->name) == name.len && !memcmp(e->name, name.ptr, name.len)) return e;
    }
    return NULL;
}

void* cx_lib_find(const cx_Library* library, cx_str name, const char* type, cx_u64 shape, cx_String* error) {
    const lib* l = (const lib*)library->p;
    *error = (cx_String){0};
    const cx_lib_export* e = lookup(l, name);
    if (e && e->shape == shape) return e->fn;
    cx_str path = {l->path.ptr, l->path.len};
    add_str(error, path);
    if (!e) {
        add(error, " exports no function `");
        add_str(error, name);
        add(error, "`");
    } else if (strcmp(e->type, type) != 0) {
        add(error, "'s `");
        add_str(error, name);
        add(error, "` is `");
        add(error, e->type);
        add(error, "`, not `");
        add(error, type);
        add(error, "`");
    } else {
        add(error, "'s `");
        add_str(error, name);
        add(error, "` is `");
        add(error, type);
        add(error, "` here too, but a type in it isn't the same in the two builds; build them from the same source");
    }
    return NULL;
}

cx_bool cx_lib_has(const cx_Library* library, cx_str name) { return lookup((const lib*)library->p, name) != NULL; }

cx_str cx_lib_path(const cx_Library* library) {
    const lib* l = (const lib*)library->p;
    return (cx_str){l->path.ptr, l->path.len};
}

void cx_drop_Library(cx_Library* library) {
    lib* l = (lib*)library->p;
    if (!l) return;
    cx_drop_String(&l->path);
    cx_free(l, sizeof(lib));
    library->p = NULL;
}

void cx_lib_close(cx_Library* library) {
    lib* l = (lib*)library->p;
    if (!l) return;
    void* h = l->handle;
    cx_drop_Library(library);
    unload(h);
}
