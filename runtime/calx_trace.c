/* Stack traces for panics: the Calx functions the panic happened in, innermost first, each with
 * the line it was on, as Java writes them: `\tat t.Box.get(t.calx:7)`. A call that a release
 * build inlined is a frame of its own, as it would be in a debug build.
 *
 * Windows asks dbghelp, which reads the program's PDB, and loads it only when a panic needs it,
 * so starting a program doesn't, and the PDB of each library the program opened (D123). Linux
 * reads the ELF symbol table and the DWARF of the program, or of the library a frame is in,
 * itself, written from the DWARF 4 and 5 standards: the line tables, and for inlined calls, the
 * compile units' entries. Only frames whose line is in a .calx file are shown, so the
 * runtime's C and the C library's frames aren't. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "calx_rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 128

static int ends_with(const char* s, const char* end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && memcmp(s + n - m, end, m) == 0;
}

/* The Calx name of a function from its C name and its file. Calx names a function in C after its
 * module's path with '_' between the parts, then "__" before each part of its own name: `t.Box.get`
 * is `t__Box__get`, and `std.lz4.compress_into` is `std_lz4__compress_into`. The module comes
 * from the file, std/lz4.calx, where its path matches; a `parallel for` body, `cx_par_t__main_1`,
 * is shown as the function it's in. What follows a '.', which the optimizer adds to a function it
 * moved or split, isn't part of it. 0 for a function that isn't Calx's. */
static int calx_name(const char* c, const char* file, char* out, size_t cap) {
    char name[256];
    size_t len = strcspn(c, ".");
    if (len >= sizeof name) len = sizeof name - 1;
    memcpy(name, c, len);
    name[len] = 0;
    if (strncmp(name, "cx_par_", 7) == 0) {
        memmove(name, name + 7, len - 6);
        len -= 7;
        while (len && name[len - 1] >= '0' && name[len - 1] <= '9') len--;
        if (len && name[len - 1] == '_') len--;
        name[len] = 0;
    } else if (strncmp(name, "cx_", 3) == 0) {
        return 0;
    }
    const char* own = strstr(name, "__");
    if (!own || own == name) return 0;
    size_t prefix = (size_t)(own - name);
    /* The file's folders and name, last first, against the prefix. */
    char module[256] = "";
    size_t flen = strlen(file) - 5; /* without ".calx" */
    for (size_t parts = 1, at = flen;; parts++) {
        size_t start = at;
        while (start && file[start - 1] != '/') start--;
        size_t span_len = flen - start;
        if (span_len == prefix) {
            int same = 1;
            for (size_t i = 0; i < prefix && same; i++) {
                char f = file[start + i] == '/' ? '_' : file[start + i];
                same = f == name[i];
            }
            if (same && span_len < sizeof module) {
                for (size_t i = 0; i < span_len; i++) module[i] = file[start + i] == '/' ? '.' : file[start + i];
                module[span_len] = 0;
                break;
            }
        }
        if (span_len > prefix || start == 0) break;
        at = start - 1;
    }
    if (!module[0]) {
        memcpy(module, name, prefix);
        module[prefix] = 0;
    }
    size_t j = (size_t)snprintf(out, cap, "%s", module);
    for (const char* p = own; *p && j + 1 < cap; p++) {
        if (p[0] == '_' && p[1] == '_') {
            out[j++] = '.';
            p++;
        } else {
            out[j++] = *p;
        }
    }
    out[j < cap ? j : cap - 1] = 0;
    return 1;
}

/* The path from the current folder when the file is under it, else the whole path. */
static const char* shorten(const char* path, const char* cwd) {
    size_t n = strlen(cwd);
    if (n == 0 || strlen(path) <= n || path[n] != '/') return path;
    for (size_t i = 0; i < n; i++) {
        char a = path[i], b = cwd[i];
#ifdef _WIN32
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
        if (a != b) return path;
    }
    return path + n + 1;
}

/* The stack's frames, innermost first, the runtime's and the C library's too, inlined calls each a
 * frame of their own, before choosing what to show. */
#define MAX_FOUND 256

typedef struct {
    char c_name[256];
    char file[1024];
    unsigned long long line;
} found_frame;

typedef struct {
    found_frame* list;
    int n;
    char cwd[4096];
} found;

static void frame(found* f, const char* c_name, const char* file, unsigned long long line) {
    if (f->n >= MAX_FOUND) return;
    found_frame* x = &f->list[f->n++];
    snprintf(x->c_name, sizeof x->c_name, "%s", c_name);
    snprintf(x->file, sizeof x->file, "%s", file);
    x->line = line;
}

/* Whether a C name is `name`, or `name` with what the optimizer adds after a '.'. */
static int is_named(const char* c, const char* name) {
    size_t n = strlen(name);
    return strncmp(c, name, n) == 0 && (c[n] == 0 || c[n] == '.');
}

/* After a frame, the same frame `n` more times, as a recursion makes: once more is written again,
 * and more than that says how many. */
static void repeated(void (*text)(const char*), const char* line, int n) {
    if (n == 1) {
        text(line);
    } else if (n > 1) {
        char out[64];
        snprintf(out, sizeof out, "\t... repeated %d more times\n", n);
        text(out);
    }
}

static void show_range(void (*text)(const char*), const found* f, int from, int to) {
    char last[1536] = "";
    int same = 0;
    for (int i = from; i < to; i++) {
        const found_frame* x = &f->list[i];
        char name[512], out[1536];
        if (!ends_with(x->file, ".calx") || !calx_name(x->c_name, x->file, name, sizeof name)) continue;
        snprintf(out, sizeof out, "\tat %s(%s:%llu)\n", name, shorten(x->file, f->cwd), x->line);
        if (strcmp(out, last) == 0) {
            same++;
            continue;
        }
        repeated(text, last, same);
        same = 0;
        text(out);
        memcpy(last, out, sizeof last);
    }
    repeated(text, last, same);
}

static void found_frames(found* f);
static void name_frames(found* f, void* const* frames, int n);

/* With CALX_TRACE_DEBUG=1, what the system's tables say of each frame, on standard error: for
 * bringing traces up on a new system, such as Windows on Arm (CLX-276) or macOS (CLX-278). */
static int trace_debug(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("CALX_TRACE_DEBUG");
        on = v && v[0] == '1';
    }
    return on;
}

/* The first frame from `from` on that is a chunk's body: called by the runtime's `par_claim`, not
 * by a loop that runs where it is. -1 if there's none. */
static int chunk_body(const found* f, int from) {
    for (int i = from; i + 1 < f->n; i++) {
        if (strncmp(f->list[i].c_name, "cx_par_", 7) == 0 && is_named(f->list[i + 1].c_name, "par_claim")) return i;
    }
    return -1;
}

static int find(const found* f, int from, const char* name) {
    for (int i = from; i < f->n; i++) {
        if (is_named(f->list[i].c_name, name)) return i;
    }
    return -1;
}

/* A `parallel for` runs its chunks on whichever threads are free, so no one thread's stack holds
 * a panic's frames from the innermost call out to `main`. Each part comes from the thread that
 * has it, in order, so the trace is the one a single thread would show:
 *   0, no chunk on this thread: every frame.
 *   1, a panic in a chunk: the frames down to the chunk's body.
 *   2, on the thread that started the outermost loop: the frames outside it. Its chunks may be
 *      on the stack, so the loop is the first `cx_parallel` from the outermost `par_claim` on.
 *   3, on the thread that started an inner loop, which ran in the chunk of the loop around it:
 *      the frames from the inner loop to that chunk's body. `ending` is how many inner loops this
 *      thread is reporting a panic in, this one too, each with a `par_inner` on the stack, so
 *      this one's is the `ending`th from the innermost. */
void cx_trace_write(void (*text)(const char*), int mode, int ending) {
    found f;
    f.n = 0;
    f.cwd[0] = 0;
    f.list = malloc(MAX_FOUND * sizeof *f.list);
    if (!f.list) return;
    found_frames(&f);
    if (mode == 0) {
        show_range(text, &f, 0, f.n);
    } else if (mode == 1) {
        int body = chunk_body(&f, 0);
        show_range(text, &f, 0, body >= 0 ? body + 1 : f.n);
    } else if (mode == 2) {
        int claim = -1;
        for (int i = find(&f, 0, "par_claim"); i >= 0; i = find(&f, i + 1, "par_claim")) claim = i;
        int loop = find(&f, claim >= 0 ? claim : 0, "cx_parallel");
        if (loop >= 0) show_range(text, &f, loop + 1, f.n);
    } else {
        int inner = -1;
        for (int k = 0; k < ending; k++) {
            inner = find(&f, inner + 1, "par_inner");
            if (inner < 0) break;
        }
        if (inner >= 0) {
            int body = chunk_body(&f, inner + 1);
            show_range(text, &f, inner + 1, body >= 0 ? body + 1 : f.n);
        }
    }
    free(f.list);
}

/* The Calx frames of a stack taken earlier, as return addresses, the innermost first, as a panic's
 * trace writes them: where a debug build's leaked blocks were allocated (CLX-261). */
void cx_trace_frames(void (*text)(const char*), void* const* frames, int n) {
    found f;
    f.n = 0;
    f.cwd[0] = 0;
    f.list = malloc(MAX_FOUND * sizeof *f.list);
    if (!f.list) return;
    name_frames(&f, frames, n);
    show_range(text, &f, 0, f.n);
    free(f.list);
}


#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

typedef DWORD(WINAPI* set_options_fn)(DWORD);
typedef BOOL(WINAPI* initialize_fn)(HANDLE, PCWSTR, BOOL);
typedef BOOL(WINAPI* search_fn)(HANDLE, PCWSTR);
typedef BOOL(WINAPI* from_addr_fn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI* line_fn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64);
typedef DWORD(WINAPI* inline_count_fn)(HANDLE, DWORD64);
typedef BOOL(WINAPI* inline_query_fn)(HANDLE, DWORD64, DWORD, DWORD64, DWORD64, LPDWORD, LPDWORD);
typedef BOOL(WINAPI* inline_from_fn)(HANDLE, DWORD64, ULONG, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI* inline_line_fn)(HANDLE, DWORD64, ULONG, DWORD64, PDWORD, PIMAGEHLP_LINEW64);

static SRWLOCK trace_lock = SRWLOCK_INIT;

/* UTF-16 to UTF-8, with '/' between folders. */
static void utf8_path(const wchar_t* w, char* out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
    for (char* p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

typedef union {
    SYMBOL_INFOW info;
    char room[sizeof(SYMBOL_INFOW) + 256 * sizeof(WCHAR)];
} symbol;

static void clear_symbol(symbol* s) {
    memset(s, 0, sizeof *s);
    s->info.SizeOfStruct = sizeof(SYMBOL_INFOW);
    s->info.MaxNameLen = 255;
}

/* The folders of the program and of each library the frames are in, for dbghelp to find their
 * PDBs in: `calx build` puts a PDB beside what it built (D123). */
static void module_folders(void* const* frames, USHORT n, wchar_t* out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (USHORT i = 0; i < n; i++) {
        HMODULE m;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)frames[i], &m)) {
            continue;
        }
        wchar_t path[MAX_PATH + 1];
        DWORD k = GetModuleFileNameW(m, path, MAX_PATH);
        if (k == 0 || k >= MAX_PATH) continue;
        while (k > 0 && path[k - 1] != L'\\' && path[k - 1] != L'/') k--;
        if (k == 0) continue;
        path[k - 1] = 0;
        /* Each folder once. */
        size_t plen = wcslen(path);
        int seen = 0;
        for (const wchar_t* at = out; *at;) {
            const wchar_t* end = wcschr(at, L';');
            size_t alen = end ? (size_t)(end - at) : wcslen(at);
            if (alen == plen && _wcsnicmp(at, path, plen) == 0) seen = 1;
            at += alen + (end ? 1 : 0);
        }
        if (seen || len + plen + 2 > cap) continue;
        if (len) out[len++] = L';';
        memcpy(out + len, path, plen * sizeof(wchar_t));
        len += plen;
        out[len] = 0;
    }
}

/* A frame with no line, such as one in the runtime, is kept for its name. */
static void add(found* f, const symbol* s, const IMAGEHLP_LINEW64* line) {
    char name[768], file[3 * MAX_PATH];
    utf8_path(s->info.Name, name, sizeof name);
    if (line->FileName) utf8_path(line->FileName, file, sizeof file);
    else file[0] = 0;
    frame(f, name, file, line->FileName ? line->LineNumber : 0);
}

static void found_frames(found* f) {
    void* frames[MAX_FRAMES];
    int n = cx_stack_capture(frames, MAX_FRAMES, 0);
    name_frames(f, frames, n);
}

typedef BOOL(WINAPI* module_fn)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);

static void name_frames(found* f, void* const* frames, int n) {
    AcquireSRWLockExclusive(&trace_lock);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) goto done;
    set_options_fn set_options = (set_options_fn)(void*)GetProcAddress(dbghelp, "SymSetOptions");
    initialize_fn initialize = (initialize_fn)(void*)GetProcAddress(dbghelp, "SymInitializeW");
    search_fn set_search = (search_fn)(void*)GetProcAddress(dbghelp, "SymSetSearchPathW");
    from_addr_fn from_addr = (from_addr_fn)(void*)GetProcAddress(dbghelp, "SymFromAddrW");
    line_fn line_of = (line_fn)(void*)GetProcAddress(dbghelp, "SymGetLineFromAddrW64");
    /* Inlined calls, which dbghelp has had since Windows 8. */
    inline_count_fn inline_count = (inline_count_fn)(void*)GetProcAddress(dbghelp, "SymAddrIncludeInlineTrace");
    inline_query_fn inline_query = (inline_query_fn)(void*)GetProcAddress(dbghelp, "SymQueryInlineTrace");
    inline_from_fn inline_from = (inline_from_fn)(void*)GetProcAddress(dbghelp, "SymFromInlineContextW");
    inline_line_fn inline_line = (inline_line_fn)(void*)GetProcAddress(dbghelp, "SymGetLineFromInlineContextW");
    if (!set_options || !initialize || !from_addr || !line_of) goto done;
    HANDLE process = GetCurrentProcess();
    set_options(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    /* Once a process: a panic in a `parallel for` takes a trace on more than one thread. */
    static int ready;
    if (!ready && !initialize(process, NULL, TRUE)) goto done;
    ready = 1;
    /* Set before any symbols load, which deferred loading leaves until a frame needs them. */
    static wchar_t folders[8192];
    module_folders(frames, (USHORT)n, folders, sizeof folders / sizeof folders[0]);
    if (set_search && folders[0]) set_search(process, folders);
    wchar_t wcwd[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, wcwd) == 0) wcwd[0] = 0;
    utf8_path(wcwd, f->cwd, sizeof f->cwd);
    int debug = trace_debug();
    module_fn module_info = debug ? (module_fn)(void*)GetProcAddress(dbghelp, "SymGetModuleInfoW64") : NULL;
    if (debug) fprintf(stderr, "trace: %d frames, symbols searched for in %ls\n", n, folders);
    for (int i = 0; i < n; i++) {
        /* A return address is just past its call, which may end the call's line. */
        DWORD64 at = (DWORD64)(uintptr_t)frames[i] - 1;
        symbol sym;
        IMAGEHLP_LINEW64 line;
        DWORD64 offset;
        DWORD column;
        if (debug) {
            clear_symbol(&sym);
            memset(&line, 0, sizeof line);
            line.SizeOfStruct = sizeof line;
            BOOL named = from_addr(process, at, &offset, &sym.info);
            DWORD name_error = named ? 0 : GetLastError();
            BOOL lined = line_of(process, at, &column, &line);
            DWORD line_error = lined ? 0 : GetLastError();
            IMAGEHLP_MODULEW64 m;
            memset(&m, 0, sizeof m);
            m.SizeOfStruct = sizeof m;
            BOOL moduled = module_info && module_info(process, at, &m);
            fprintf(stderr, "trace: %2d %p %ls (%lu) %ls:%lu (%lu) in %ls, symbols %d from %ls, %lu inlined\n", i, frames[i],
                    named ? sym.info.Name : L"?", (unsigned long)name_error, lined ? line.FileName : L"?",
                    lined ? (unsigned long)line.LineNumber : 0ul, (unsigned long)line_error, moduled ? m.ModuleName : L"?",
                    moduled ? (int)m.SymType : -1, moduled ? m.LoadedPdbName : L"?",
                    inline_count ? (unsigned long)inline_count(process, at) : 0ul);
        }
        /* The calls inlined at this address, innermost first, each at its own line. */
        DWORD inlined = inline_count && inline_query && inline_from && inline_line ? inline_count(process, at) : 0;
        DWORD context = 0, index = 0;
        if (inlined && inline_query(process, at, 0, at, at, &context, &index)) {
            for (DWORD k = 0; k < inlined; k++, context++) {
                clear_symbol(&sym);
                memset(&line, 0, sizeof line);
                line.SizeOfStruct = sizeof line;
                if (inline_from(process, at, context, &offset, &sym.info)) {
                    if (!inline_line(process, at, context, 0, &column, &line)) line.FileName = NULL;
                    add(f, &sym, &line);
                }
            }
        }
        clear_symbol(&sym);
        memset(&line, 0, sizeof line);
        line.SizeOfStruct = sizeof line;
        if (from_addr(process, at, &offset, &sym.info)) {
            if (!line_of(process, at, &column, &line)) line.FileName = NULL;
            add(f, &sym, &line);
        }
    }
done:
    ReleaseSRWLockExclusive(&trace_lock);
}

#else

/* Linux and macOS read the DWARF clang writes: on Linux from the ELF file itself, on macOS from
 * the file dsymutil gathers it into beside the program (CLX-278), as its linker leaves it in the
 * objects. The reader is the same; only finding the sections and the load address differ. */
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <elf.h>
#include <link.h>
#endif

static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const cx_u8* p;
    size_t n;
} span;

/* A reader that stops at the end of its bytes, and remembers that it tried to go past. */
typedef struct {
    const cx_u8* p;
    const cx_u8* end;
    int bad;
} reader;

static cx_u64 fixed(reader* r, int n) {
    if (r->end - r->p < n) {
        r->bad = 1;
        r->p = r->end;
        return 0;
    }
    cx_u64 v = 0;
    for (int i = 0; i < n; i++) v |= (cx_u64)r->p[i] << (8 * i);
    r->p += n;
    return v;
}

static void skip(reader* r, cx_u64 n) {
    if ((cx_u64)(r->end - r->p) < n) {
        r->bad = 1;
        r->p = r->end;
    } else {
        r->p += n;
    }
}

static cx_u64 uleb(reader* r) {
    cx_u64 v = 0;
    for (int shift = 0;; shift += 7) {
        if (r->p >= r->end) {
            r->bad = 1;
            return 0;
        }
        cx_u8 b = *r->p++;
        if (shift < 64) v |= (cx_u64)(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
    }
}

static cx_i64 sleb(reader* r) {
    cx_u64 v = 0;
    int shift = 0;
    cx_u8 b;
    do {
        if (r->p >= r->end) {
            r->bad = 1;
            return 0;
        }
        b = *r->p++;
        if (shift < 64) v |= (cx_u64)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= ~(cx_u64)0 << shift;
    return (cx_i64)v;
}

static const char* cstr(reader* r) {
    const cx_u8* s = r->p;
    while (r->p < r->end && *r->p) r->p++;
    if (r->p >= r->end) {
        r->bad = 1;
        return "";
    }
    r->p++;
    return (const char*)s;
}

/* The string at `offset` in a string section, or "" if it isn't one. */
static const char* string_at(span s, cx_u64 offset) {
    if (offset >= s.n || !memchr(s.p + offset, 0, s.n - offset)) return "";
    return (const char*)s.p + offset;
}

/* The executable's debug sections. */
typedef struct {
    span line, line_str, str, info, abbrev, addr, str_offsets, rnglists;
} dwarf;

/* --- line tables, .debug_line ------------------------------------------------------------ */

typedef struct {
    const char* path;
    cx_u64 dir;
} file_entry;

/* A line table's header: what its program needs, and its directories and files. */
typedef struct {
    int version, dwarf64, line_base, line_range, opcode_base;
    cx_u64 min_inst;
    const cx_u8* lengths;
    const cx_u8* program;
    const cx_u8* end;
    file_entry* dirs;
    file_entry* files;
    long dir_count, file_count;
    int first_file; /* the number of the table's first file: 0 from DWARF 5, 1 before */
} line_header;

static void free_header(line_header* h) {
    free(h->dirs);
    free(h->files);
    h->dirs = h->files = NULL;
}

/* Reads one value of a line table's directory or file entry. A path gives its text, anything
 * else its number in *num. -1 for a form a line table shouldn't have. */
static int entry_value(reader* r, const dwarf* d, cx_u64 form, int dwarf64, const char** text, cx_u64* num) {
    *text = NULL;
    *num = 0;
    switch (form) {
    case 0x08: *text = cstr(r); return 0;                                           /* string */
    case 0x1f: *text = string_at(d->line_str, fixed(r, dwarf64 ? 8 : 4)); return 0; /* line_strp */
    case 0x0e: *text = string_at(d->str, fixed(r, dwarf64 ? 8 : 4)); return 0;      /* strp */
    case 0x0f: *num = uleb(r); return 0;                                            /* udata */
    case 0x0b: *num = fixed(r, 1); return 0;                                        /* data1 */
    case 0x05: *num = fixed(r, 2); return 0;                                        /* data2 */
    case 0x06: *num = fixed(r, 4); return 0;                                        /* data4 */
    case 0x07: *num = fixed(r, 8); return 0;                                        /* data8 */
    case 0x1e: skip(r, 16); return 0;                                               /* data16 */
    case 0x09: skip(r, uleb(r)); return 0;                                          /* block */
    default: return -1;
    }
}

/* Reads a DWARF 5 table of directories or files: its entry format, then its entries. Gives the
 * count, with the entries in a list the caller frees, or -1. */
static long read_entries(reader* r, const dwarf* d, int dwarf64, file_entry** out) {
    cx_u64 formats[16][2];
    unsigned count = (unsigned)fixed(r, 1);
    if (count > 16) return -1;
    for (unsigned i = 0; i < count; i++) {
        formats[i][0] = uleb(r);
        formats[i][1] = uleb(r);
    }
    cx_u64 n = uleb(r);
    if (r->bad || n > 1000000) return -1;
    file_entry* list = calloc(n ? n : 1, sizeof *list);
    if (!list) return -1;
    for (cx_u64 k = 0; k < n; k++) {
        list[k].path = "";
        for (unsigned i = 0; i < count; i++) {
            const char* text;
            cx_u64 num;
            if (entry_value(r, d, formats[i][1], dwarf64, &text, &num) < 0 || r->bad) {
                free(list);
                return -1;
            }
            if (formats[i][0] == 1 && text) list[k].path = text; /* DW_LNCT_path */
            if (formats[i][0] == 2) list[k].dir = num;           /* DW_LNCT_directory_index */
        }
    }
    *out = list;
    return (long)n;
}

/* Reads the header of the line table at `offset` in .debug_line. 0, or -1. */
static int read_header(const dwarf* d, cx_u64 offset, line_header* h) {
    memset(h, 0, sizeof *h);
    if (offset >= d->line.n) return -1;
    reader r = {d->line.p + offset, d->line.p + d->line.n, 0};
    cx_u64 length = fixed(&r, 4);
    if (length == 0xffffffffu) {
        length = fixed(&r, 8);
        h->dwarf64 = 1;
    }
    if (r.bad || length > (cx_u64)(r.end - r.p)) return -1;
    h->end = r.p + length;
    r.end = h->end;
    h->version = (int)fixed(&r, 2);
    if (h->version < 2 || h->version > 5) return -1;
    if (h->version >= 5) fixed(&r, 2); /* address and segment selector sizes */
    cx_u64 header_length = fixed(&r, h->dwarf64 ? 8 : 4);
    if (r.bad || header_length > (cx_u64)(r.end - r.p)) return -1;
    h->program = r.p + header_length;
    h->min_inst = fixed(&r, 1);
    if (h->version >= 4) fixed(&r, 1); /* operations per instruction, which is 1 but on VLIW */
    fixed(&r, 1);                      /* is_stmt's default */
    h->line_base = (int)(signed char)fixed(&r, 1);
    h->line_range = (int)fixed(&r, 1);
    h->opcode_base = (int)fixed(&r, 1);
    if (r.bad || h->line_range == 0 || h->opcode_base == 0 || r.end - r.p < h->opcode_base - 1) return -1;
    h->lengths = r.p;
    r.p += h->opcode_base - 1;
    if (h->version >= 5) {
        h->dir_count = read_entries(&r, d, h->dwarf64, &h->dirs);
        if (h->dir_count < 0) return -1;
        h->file_count = read_entries(&r, d, h->dwarf64, &h->files);
        if (h->file_count < 0) {
            free_header(h);
            return -1;
        }
        h->first_file = 0;
        return 0;
    }
    /* Directory 0 is the unit's own, which only .debug_info names. */
    size_t room = 16;
    h->dirs = calloc(room, sizeof *h->dirs);
    if (!h->dirs) return -1;
    h->dirs[h->dir_count++].path = "";
    for (;;) {
        const char* s = cstr(&r);
        if (r.bad || !*s) break;
        if ((size_t)h->dir_count == room) {
            file_entry* more = realloc(h->dirs, (room *= 2) * sizeof *more);
            if (!more) break;
            h->dirs = more;
        }
        h->dirs[h->dir_count++].path = s;
    }
    room = 16;
    h->files = calloc(room, sizeof *h->files);
    if (!h->files) {
        free_header(h);
        return -1;
    }
    for (;;) {
        const char* s = cstr(&r);
        if (r.bad || !*s) break;
        cx_u64 dir = uleb(&r);
        uleb(&r);
        uleb(&r);
        if ((size_t)h->file_count == room) {
            file_entry* more = realloc(h->files, (room *= 2) * sizeof *more);
            if (!more) break;
            h->files = more;
        }
        h->files[h->file_count].path = s;
        h->files[h->file_count++].dir = dir;
    }
    h->first_file = 1;
    return 0;
}

/* The path of file `number` in a line table. In DWARF 5 directory 0 is the unit's folder, and
 * the others may be under it. */
static void file_path(const line_header* h, cx_u64 number, char* path, size_t cap) {
    long k = (long)number - h->first_file;
    path[0] = 0;
    if (k < 0 || k >= h->file_count) return;
    const char* name = h->files[k].path;
    cx_u64 dir = h->files[k].dir;
    const char* folder = (long)dir < h->dir_count ? h->dirs[dir].path : "";
    const char* top = h->version >= 5 && h->dir_count > 0 && dir != 0 ? h->dirs[0].path : "";
    if (name[0] == '/' || !folder[0]) snprintf(path, cap, "%s", name);
    else if (folder[0] == '/' || !top[0]) snprintf(path, cap, "%s/%s", folder, name);
    else snprintf(path, cap, "%s/%s/%s", top, folder, name);
}

/* Runs a line table's program to the row that covers `at`: rows of address, file and line, in
 * sequences of rising addresses, each covering the addresses up to the next. 1 with the file's
 * number and the line, else 0. */
static int run_program(const line_header* h, cx_u64 at, cx_u64* file_out, cx_u64* line_out) {
    reader p = {h->program, h->end, 0};
    cx_u64 address = 0, file = 1, line = 1;
    cx_u64 row_address = 0, row_file = 0, row_line = 0;
    int have_row = 0;
    while (p.p < p.end && !p.bad) {
        int op = (int)fixed(&p, 1);
        int emit = 0, end_sequence = 0;
        if (op >= h->opcode_base) {
            int adjusted = op - h->opcode_base;
            address += (cx_u64)(adjusted / h->line_range) * h->min_inst;
            line += (cx_u64)(cx_i64)(h->line_base + adjusted % h->line_range);
            emit = 1;
        } else if (op == 0) {
            cx_u64 n = uleb(&p);
            if (p.bad || n == 0 || n > (cx_u64)(p.end - p.p)) break;
            const cx_u8* next = p.p + n;
            int sub = (int)fixed(&p, 1);
            if (sub == 1) { /* end_sequence */
                emit = 1;
                end_sequence = 1;
            } else if (sub == 2) { /* set_address */
                address = fixed(&p, (int)(n - 1 > 8 ? 8 : n - 1));
            }
            p.p = next;
        } else {
            switch (op) {
            case 1: emit = 1; break;                                                              /* copy */
            case 2: address += uleb(&p) * h->min_inst; break;                                     /* advance_pc */
            case 3: line += (cx_u64)sleb(&p); break;                                              /* advance_line */
            case 4: file = uleb(&p); break;                                                       /* set_file */
            case 8: address += (cx_u64)((255 - h->opcode_base) / h->line_range) * h->min_inst; break; /* const_add_pc */
            case 9: address += fixed(&p, 2); break;                                               /* fixed_advance_pc */
            default:
                for (int k = 0; k < h->lengths[op - 1]; k++) uleb(&p);
                break;
            }
        }
        if (!emit) continue;
        if (have_row && row_address <= at && at < address) {
            *file_out = row_file;
            *line_out = row_line;
            return 1;
        }
        if (end_sequence) {
            have_row = 0;
            address = 0;
            file = 1;
            line = 1;
        } else {
            have_row = 1;
            row_address = address;
            row_file = file;
            row_line = line;
        }
    }
    return 0;
}

/* The file and line of `at`, from whichever line table covers it. */
static int find_line(const dwarf* d, cx_u64 at, char* path, size_t cap, cx_u64* line) {
    cx_u64 offset = 0;
    while (offset < d->line.n) {
        line_header h;
        if (read_header(d, offset, &h) != 0) return 0;
        cx_u64 file;
        int found = run_program(&h, at, &file, line);
        if (found) file_path(&h, file, path, cap);
        offset = (cx_u64)(h.end - d->line.p);
        free_header(&h);
        if (found) return 1;
    }
    return 0;
}

/* --- inlined calls, .debug_info ---------------------------------------------------------- */

/* What release builds inline is in .debug_info: each function's entry holds an entry for each
 * call inlined into it, with the addresses its code took and the line of the call. Only DWARF 5
 * compile units are read, which is what clang writes. */

enum {
    TAG_COMPILE_UNIT = 0x11,
    TAG_SUBPROGRAM = 0x2e,
    TAG_INLINED = 0x1d,
    AT_NAME = 0x03,
    AT_STMT_LIST = 0x10,
    AT_LOW_PC = 0x11,
    AT_HIGH_PC = 0x12,
    AT_ABSTRACT_ORIGIN = 0x31,
    AT_SPECIFICATION = 0x47,
    AT_RANGES = 0x55,
    AT_CALL_FILE = 0x58,
    AT_CALL_LINE = 0x59,
    AT_LINKAGE_NAME = 0x6e,
    AT_STR_OFFSETS_BASE = 0x72,
    AT_ADDR_BASE = 0x73,
    AT_RNGLISTS_BASE = 0x74,
};

typedef struct {
    cx_u64 attr, form;
    cx_i64 implicit;
} attr_spec;

typedef struct {
    cx_u64 code, tag;
    int children;
    size_t first, count; /* its attributes in the unit's list */
} abbrev;

typedef struct {
    abbrev* list;
    size_t n, room;
    attr_spec* attrs;
    size_t n_attrs, attr_room;
} abbrevs;

static void free_abbrevs(abbrevs* a) {
    free(a->list);
    free(a->attrs);
}

static int read_abbrevs(const dwarf* d, cx_u64 offset, abbrevs* a) {
    memset(a, 0, sizeof *a);
    if (offset >= d->abbrev.n) return -1;
    reader r = {d->abbrev.p + offset, d->abbrev.p + d->abbrev.n, 0};
    for (;;) {
        cx_u64 code = uleb(&r);
        if (r.bad) return -1;
        if (code == 0) return 0;
        if (a->n == a->room) {
            a->room = a->room ? a->room * 2 : 64;
            abbrev* more = realloc(a->list, a->room * sizeof *more);
            if (!more) return -1;
            a->list = more;
        }
        abbrev* ab = &a->list[a->n++];
        ab->code = code;
        ab->tag = uleb(&r);
        ab->children = (int)fixed(&r, 1);
        ab->first = a->n_attrs;
        ab->count = 0;
        for (;;) {
            cx_u64 attr = uleb(&r), form = uleb(&r);
            cx_i64 implicit = form == 0x21 ? sleb(&r) : 0;
            if (r.bad) return -1;
            if (attr == 0 && form == 0) break;
            if (a->n_attrs == a->attr_room) {
                a->attr_room = a->attr_room ? a->attr_room * 2 : 256;
                attr_spec* more = realloc(a->attrs, a->attr_room * sizeof *more);
                if (!more) return -1;
                a->attrs = more;
            }
            a->attrs[a->n_attrs++] = (attr_spec){attr, form, implicit};
            ab->count++;
        }
    }
}

static const abbrev* find_abbrev(const abbrevs* a, cx_u64 code) {
    if (code >= 1 && code <= a->n && a->list[code - 1].code == code) return &a->list[code - 1];
    for (size_t i = 0; i < a->n; i++) {
        if (a->list[i].code == code) return &a->list[i];
    }
    return NULL;
}

/* A compile unit, and the bases its indexed forms count from. */
typedef struct {
    const cx_u8* start; /* its header, which references are from */
    const cx_u8* end;
    int dwarf64, addr_size;
    cx_u64 str_offsets_base, addr_base, rnglists_base, low_pc, stmt_list;
    abbrevs ab;
} unit;

enum { V_NONE, V_CONST, V_ADDR, V_ADDRX, V_STR, V_STRX, V_REF, V_OFFSET, V_RNGLISTX };

typedef struct {
    int kind;
    cx_u64 u;
    const char* s;
} value;

static int read_value(reader* r, const unit* u, cx_u64 form, cx_i64 implicit, const dwarf* d, value* v) {
    int off = u->dwarf64 ? 8 : 4;
    v->kind = V_NONE;
    v->u = 0;
    v->s = NULL;
    switch (form) {
    case 0x01: v->kind = V_ADDR; v->u = fixed(r, u->addr_size); break;
    case 0x03: skip(r, fixed(r, 2)); break;
    case 0x04: skip(r, fixed(r, 4)); break;
    case 0x05: v->kind = V_CONST; v->u = fixed(r, 2); break;
    case 0x06: v->kind = V_CONST; v->u = fixed(r, 4); break;
    case 0x07: v->kind = V_CONST; v->u = fixed(r, 8); break;
    case 0x08: v->kind = V_STR; v->s = cstr(r); break;
    case 0x09: case 0x18: skip(r, uleb(r)); break;
    case 0x0a: skip(r, fixed(r, 1)); break;
    case 0x0b: case 0x0c: v->kind = V_CONST; v->u = fixed(r, 1); break;
    case 0x0d: v->kind = V_CONST; v->u = (cx_u64)sleb(r); break;
    case 0x0e: v->kind = V_STR; v->s = string_at(d->str, fixed(r, off)); break;
    case 0x0f: v->kind = V_CONST; v->u = uleb(r); break;
    case 0x10: fixed(r, off); break;
    case 0x11: v->kind = V_REF; v->u = fixed(r, 1); break;
    case 0x12: v->kind = V_REF; v->u = fixed(r, 2); break;
    case 0x13: v->kind = V_REF; v->u = fixed(r, 4); break;
    case 0x14: v->kind = V_REF; v->u = fixed(r, 8); break;
    case 0x15: v->kind = V_REF; v->u = uleb(r); break;
    case 0x16: return read_value(r, u, uleb(r), 0, d, v);
    case 0x17: v->kind = V_OFFSET; v->u = fixed(r, off); break;
    case 0x19: v->kind = V_CONST; v->u = 1; break;
    case 0x1a: v->kind = V_STRX; v->u = uleb(r); break;
    case 0x1b: v->kind = V_ADDRX; v->u = uleb(r); break;
    case 0x1c: fixed(r, 4); break;
    case 0x1d: fixed(r, off); break;
    case 0x1e: skip(r, 16); break;
    case 0x1f: v->kind = V_STR; v->s = string_at(d->line_str, fixed(r, off)); break;
    case 0x20: fixed(r, 8); break;
    case 0x21: v->kind = V_CONST; v->u = (cx_u64)implicit; break;
    case 0x22: uleb(r); break;
    case 0x23: v->kind = V_RNGLISTX; v->u = uleb(r); break;
    case 0x24: fixed(r, 8); break;
    case 0x25: case 0x26: case 0x27: case 0x28: v->kind = V_STRX; v->u = fixed(r, (int)(form - 0x24)); break;
    case 0x29: case 0x2a: case 0x2b: case 0x2c: v->kind = V_ADDRX; v->u = fixed(r, (int)(form - 0x28)); break;
    default: return -1;
    }
    return r->bad ? -1 : 0;
}

static cx_u64 indexed_addr(const dwarf* d, const unit* u, cx_u64 index) {
    cx_u64 at = u->addr_base + index * (cx_u64)u->addr_size;
    if (at > d->addr.n || d->addr.n - at < (cx_u64)u->addr_size) return 0;
    reader r = {d->addr.p + at, d->addr.p + d->addr.n, 0};
    return fixed(&r, u->addr_size);
}

static const char* indexed_str(const dwarf* d, const unit* u, cx_u64 index) {
    int off = u->dwarf64 ? 8 : 4;
    cx_u64 at = u->str_offsets_base + index * (cx_u64)off;
    if (at > d->str_offsets.n || d->str_offsets.n - at < (cx_u64)off) return "";
    reader r = {d->str_offsets.p + at, d->str_offsets.p + d->str_offsets.n, 0};
    return string_at(d->str, fixed(&r, off));
}

/* The attributes of an entry that a trace needs. */
typedef struct {
    cx_u64 tag;
    int children;
    value name, linkage, low, high, ranges, origin, call_file, call_line;
    value stmt_list, str_base, addr_base, rng_base;
} entry;

static int read_entry(reader* r, const unit* u, const dwarf* d, entry* e) {
    cx_u64 code = uleb(r);
    memset(e, 0, sizeof *e);
    if (r->bad) return -1;
    if (code == 0) return 0;
    const abbrev* ab = find_abbrev(&u->ab, code);
    if (!ab) return -1;
    e->tag = ab->tag;
    e->children = ab->children;
    for (size_t i = 0; i < ab->count; i++) {
        const attr_spec* s = &u->ab.attrs[ab->first + i];
        value v;
        if (read_value(r, u, s->form, s->implicit, d, &v) != 0) return -1;
        switch (s->attr) {
        case AT_NAME: e->name = v; break;
        case AT_LINKAGE_NAME: e->linkage = v; break;
        case AT_LOW_PC: e->low = v; break;
        case AT_HIGH_PC: e->high = v; break;
        case AT_RANGES: e->ranges = v; break;
        case AT_ABSTRACT_ORIGIN: case AT_SPECIFICATION: e->origin = v; break;
        case AT_CALL_FILE: e->call_file = v; break;
        case AT_CALL_LINE: e->call_line = v; break;
        case AT_STMT_LIST: e->stmt_list = v; break;
        case AT_STR_OFFSETS_BASE: e->str_base = v; break;
        case AT_ADDR_BASE: e->addr_base = v; break;
        case AT_RNGLISTS_BASE: e->rng_base = v; break;
        default: break;
        }
    }
    return 1;
}

static cx_u64 address_of(const dwarf* d, const unit* u, value v) {
    return v.kind == V_ADDRX ? indexed_addr(d, u, v.u) : v.u;
}

static const char* text_of(const dwarf* d, const unit* u, value v) {
    if (v.kind == V_STR) return v.s;
    if (v.kind == V_STRX) return indexed_str(d, u, v.u);
    return NULL;
}

/* Whether an entry's code takes in `at`: one range, or a list in .debug_rnglists. */
static int covers(const dwarf* d, const unit* u, const entry* e, cx_u64 at) {
    if (e->low.kind != V_NONE && e->high.kind != V_NONE) {
        cx_u64 low = address_of(d, u, e->low);
        cx_u64 high = e->high.kind == V_CONST ? low + e->high.u : address_of(d, u, e->high);
        return low <= at && at < high;
    }
    if (e->ranges.kind == V_NONE) return 0;
    cx_u64 offset;
    int off = u->dwarf64 ? 8 : 4;
    if (e->ranges.kind == V_RNGLISTX) {
        cx_u64 slot = u->rnglists_base + e->ranges.u * (cx_u64)off;
        if (slot > d->rnglists.n || d->rnglists.n - slot < (cx_u64)off) return 0;
        reader t = {d->rnglists.p + slot, d->rnglists.p + d->rnglists.n, 0};
        offset = u->rnglists_base + fixed(&t, off);
    } else {
        offset = e->ranges.u;
    }
    if (offset >= d->rnglists.n) return 0;
    reader r = {d->rnglists.p + offset, d->rnglists.p + d->rnglists.n, 0};
    cx_u64 base = u->low_pc;
    while (!r.bad) {
        int kind = (int)fixed(&r, 1);
        cx_u64 a = 0, b = 0;
        switch (kind) {
        case 0: return 0;                                                                  /* end_of_list */
        case 1: base = indexed_addr(d, u, uleb(&r)); continue;                              /* base_addressx */
        case 2: a = indexed_addr(d, u, uleb(&r)); b = indexed_addr(d, u, uleb(&r)); break;  /* startx_endx */
        case 3: a = indexed_addr(d, u, uleb(&r)); b = a + uleb(&r); break;                  /* startx_length */
        case 4: a = base + uleb(&r); b = base + uleb(&r); break;                            /* offset_pair */
        case 5: base = fixed(&r, u->addr_size); continue;                                   /* base_address */
        case 6: a = fixed(&r, u->addr_size); b = fixed(&r, u->addr_size); break;            /* start_end */
        case 7: a = fixed(&r, u->addr_size); b = a + uleb(&r); break;                       /* start_length */
        default: return 0;
        }
        if (a <= at && at < b) return 1;
    }
    return 0;
}

/* The C name of a function's entry, following what it's a copy of. */
static const char* entry_name(const dwarf* d, const unit* u, const entry* e, int depth) {
    const char* s = text_of(d, u, e->linkage);
    if (!s) s = text_of(d, u, e->name);
    if (s || e->origin.kind != V_REF || depth > 4) return s;
    if (e->origin.u >= (cx_u64)(u->end - u->start)) return NULL;
    reader r = {u->start + e->origin.u, u->end, 0};
    entry o;
    if (read_entry(&r, u, d, &o) != 1) return NULL;
    return entry_name(d, u, &o, depth + 1);
}

#define MAX_INLINED 32

typedef struct {
    const char* name;
    cx_u64 call_file, call_line;
} level;

/* The functions whose code `at` is in, outermost first: the function, then each call inlined
 * into it down to the innermost. Gives the count, with the line table of their unit in *h, or 0. */
static int inline_levels(const dwarf* d, cx_u64 at, level* levels, line_header* h) {
    reader r = {d->info.p, d->info.p + d->info.n, 0};
    while (r.p < r.end && !r.bad) {
        unit u;
        memset(&u, 0, sizeof u);
        u.start = r.p;
        cx_u64 length = fixed(&r, 4);
        if (length == 0xffffffffu) {
            length = fixed(&r, 8);
            u.dwarf64 = 1;
        }
        if (r.bad || length > (cx_u64)(r.end - r.p)) return 0;
        u.end = r.p + length;
        reader c = {r.p, u.end, 0};
        r.p = u.end;
        int version = (int)fixed(&c, 2);
        if (version != 5 || fixed(&c, 1) != 1) continue; /* DW_UT_compile */
        u.addr_size = (int)fixed(&c, 1);
        cx_u64 abbrev_offset = fixed(&c, u.dwarf64 ? 8 : 4);
        if (c.bad || u.addr_size != 8 || read_abbrevs(d, abbrev_offset, &u.ab) != 0) {
            free_abbrevs(&u.ab);
            continue;
        }
        entry e;
        if (read_entry(&c, &u, d, &e) != 1 || e.tag != TAG_COMPILE_UNIT) {
            free_abbrevs(&u.ab);
            continue;
        }
        u.str_offsets_base = e.str_base.u;
        u.addr_base = e.addr_base.u;
        u.rnglists_base = e.rng_base.u;
        u.stmt_list = e.stmt_list.u;
        u.low_pc = e.low.kind != V_NONE ? address_of(d, &u, e.low) : 0;
        if (!covers(d, &u, &e, at) || !e.children) {
            free_abbrevs(&u.ab);
            continue;
        }
        /* The entries in order, each function or inlined call that covers `at` one level in from
         * the last. */
        int n = 0, depth = 1, last_depth = 0;
        while (c.p < c.end && depth > 0) {
            int got = read_entry(&c, &u, d, &e);
            if (got < 0) break;
            if (got == 0) {
                depth--;
                continue;
            }
            if ((e.tag == TAG_SUBPROGRAM || e.tag == TAG_INLINED) && depth > last_depth && n < MAX_INLINED &&
                (n > 0 || e.tag == TAG_SUBPROGRAM) && covers(d, &u, &e, at)) {
                levels[n].name = entry_name(d, &u, &e, 0);
                levels[n].call_file = e.call_file.u;
                levels[n].call_line = e.call_line.u;
                n++;
                last_depth = depth;
            }
            if (e.children) depth++;
        }
        int ok = n > 0 && read_header(d, u.stmt_list, h) == 0;
        free_abbrevs(&u.ab);
        return ok ? n : 0;
    }
    return 0;
}

/* The frames at `at`, an address as the DWARF gives them, in the function `name`: the call inlined
 * innermost at the line the table gives, and each call out from it at the line it was called
 * from. With `dwarf_name`, or no `name`, the function's own name is the one its DWARF gives, as
 * macOS's nearest symbol may be another function's. One with no line is kept for its name, such
 * as a frame in the runtime. */
static void dwarf_frames(found* f, const dwarf* d, cx_u64 at, const char* name, int dwarf_name) {
    char path[4096];
    cx_u64 line;
    if (!find_line(d, at, path, sizeof path, &line)) {
        if (name && *name) frame(f, name, "", 0);
        return;
    }
    level levels[MAX_INLINED];
    line_header h;
    int depth = d->info.p ? inline_levels(d, at, levels, &h) : 0;
    if (trace_debug()) {
        fprintf(stderr, "trace: at 0x%llx %s, %s:%llu, %d levels\n", (unsigned long long)at, name ? name : "?", path,
                (unsigned long long)line, depth);
        for (int j = 0; j < depth; j++) {
            fprintf(stderr, "trace:   %d %s, called at file %llu line %llu\n", j, levels[j].name ? levels[j].name : "?",
                    (unsigned long long)levels[j].call_file, (unsigned long long)levels[j].call_line);
        }
    }
    if (depth > 0 && levels[0].name && (dwarf_name || !name)) name = levels[0].name;
    if (!name) name = "";
    if (depth <= 1) {
        frame(f, name, path, line);
    } else {
        frame(f, levels[depth - 1].name ? levels[depth - 1].name : "", path, line);
        for (int j = depth - 1; j >= 1; j--) {
            char call[4096];
            file_path(&h, levels[j].call_file, call, sizeof call);
            const char* outer = levels[j - 1].name ? levels[j - 1].name : (j == 1 ? name : "");
            frame(f, outer, call, levels[j].call_line);
        }
    }
    if (depth > 0) free_header(&h);
}

/* A program, or a library it opened (D123), mapped to read its symbols and line tables. */
typedef struct {
    uintptr_t base; /* where the system loaded it: on macOS, how far from where it was linked */
    const cx_u8* image;
    size_t size;
    span symtab, strtab;
    dwarf d;
} object;

#define MAX_OBJECTS 8

static void found_frames(found* f) {
    void* frames[MAX_FRAMES];
    int n = backtrace(frames, MAX_FRAMES);
    name_frames(f, frames, n);
}

#ifdef __APPLE__

/* A Mach-O section's name fills 16 bytes, with no 0 after it when it's that long. */
static int section_is(const char* name, const char* want) {
    size_t n = strlen(want);
    return n <= 16 && strncmp(name, want, n) == 0 && (n == 16 || name[n] == 0);
}

/* The 64-bit Mach-O image at `image`, of `size` bytes: its load commands, or NULL. */
static const struct load_command* first_command(const cx_u8* image, size_t size, uint32_t* count, const cx_u8** end) {
    const struct mach_header_64* mh = (const struct mach_header_64*)image;
    if (size < sizeof *mh || mh->magic != MH_MAGIC_64 || mh->sizeofcmds > size - sizeof *mh) return NULL;
    *count = mh->ncmds;
    *end = image + sizeof *mh + mh->sizeofcmds;
    return (const struct load_command*)(image + sizeof *mh);
}

/* The next load command after `lc`, or NULL past the last or a malformed one. */
static const struct load_command* next_command(const struct load_command* lc, const cx_u8* end) {
    const cx_u8* p = (const cx_u8*)lc;
    if ((size_t)(end - p) < sizeof *lc || lc->cmdsize < sizeof *lc || lc->cmdsize > (size_t)(end - p)) return NULL;
    p += lc->cmdsize;
    return (size_t)(end - p) >= sizeof *lc ? (const struct load_command*)p : NULL;
}

/* The UUID the linker gave an image, which its DWARF file carries too: 0 if it has none. */
static int image_uuid(const cx_u8* image, size_t size, cx_u8 uuid[16]) {
    uint32_t count;
    const cx_u8* end;
    const struct load_command* lc = first_command(image, size, &count, &end);
    for (uint32_t i = 0; lc && i < count; i++, lc = next_command(lc, end)) {
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command) && (size_t)(end - (const cx_u8*)lc) >= sizeof(struct uuid_command)) {
            memcpy(uuid, ((const struct uuid_command*)lc)->uuid, 16);
            return 1;
        }
    }
    return 0;
}

/* Maps the DWARF file at `path`, dsymutil's, if it's the one for the image with `uuid`, and finds
 * its line tables in its __DWARF segment. */
static int open_dwarf(object* o, const char* path, const cx_u8 uuid[16], uintptr_t slide) {
    memset(o, 0, sizeof *o);
    o->base = slide;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    const cx_u8* image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (image == MAP_FAILED) return 0;
    cx_u8 has[16];
    uint32_t count;
    const cx_u8* end;
    const struct load_command* lc = first_command(image, size, &count, &end);
    if (!lc || !image_uuid(image, size, has) || memcmp(has, uuid, 16) != 0) {
        munmap((void*)image, size);
        return 0;
    }
    o->image = image;
    o->size = size;
    dwarf* d = &o->d;
    for (uint32_t i = 0; lc && i < count; i++, lc = next_command(lc, end)) {
        if (lc->cmd != LC_SEGMENT_64 || lc->cmdsize < sizeof(struct segment_command_64)) continue;
        const struct segment_command_64* sg = (const struct segment_command_64*)lc;
        if (strncmp(sg->segname, "__DWARF", 16) != 0) continue;
        if (sg->nsects > (lc->cmdsize - sizeof *sg) / sizeof(struct section_64)) continue;
        const struct section_64* s = (const struct section_64*)(sg + 1);
        for (uint32_t k = 0; k < sg->nsects; k++) {
            if (s[k].offset > size || s[k].size > size - s[k].offset) continue;
            span sec = {image + s[k].offset, (size_t)s[k].size};
            const char* name = s[k].sectname;
            if (section_is(name, "__debug_line")) d->line = sec;
            else if (section_is(name, "__debug_line_str")) d->line_str = sec;
            else if (section_is(name, "__debug_str")) d->str = sec;
            else if (section_is(name, "__debug_info")) d->info = sec;
            else if (section_is(name, "__debug_abbrev")) d->abbrev = sec;
            else if (section_is(name, "__debug_addr")) d->addr = sec;
            else if (section_is(name, "__debug_str_offs")) d->str_offsets = sec; /* __debug_str_offsets, cut to 16 */
            else if (section_is(name, "__debug_rnglists")) d->rnglists = sec;
        }
    }
    if (d->line.p) return 1;
    munmap((void*)image, size);
    o->image = NULL;
    return 0;
}

/* The DWARF of the image loaded at `header` from `path`: `path.dwarf`, as the cache keeps it, or
 * the `path.dSYM` bundle `calx build` writes beside a program, as debuggers find it. */
static int open_image(object* o, const void* header, const char* path) {
    memset(o, 0, sizeof *o);
    const struct mach_header_64* mh = (const struct mach_header_64*)header;
    cx_u8 uuid[16];
    if (mh->magic != MH_MAGIC_64 || !image_uuid((const cx_u8*)header, sizeof *mh + mh->sizeofcmds, uuid)) return 0;
    intptr_t slide = 0;
    int loaded = 0;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n && !loaded; i++) {
        if ((const void*)_dyld_get_image_header(i) == header) {
            slide = _dyld_get_image_vmaddr_slide(i);
            loaded = 1;
        }
    }
    if (!loaded) return 0;
    char file[4096];
    snprintf(file, sizeof file, "%s.dwarf", path);
    if (open_dwarf(o, file, uuid, (uintptr_t)slide)) return 1;
    const char* base = strrchr(path, '/');
    snprintf(file, sizeof file, "%s.dSYM/Contents/Resources/DWARF/%s", path, base ? base + 1 : path);
    return open_dwarf(o, file, uuid, (uintptr_t)slide);
}

static void name_frames(found* f, void* const* frames, int n) {
    pthread_mutex_lock(&trace_lock);
    /* Each image is opened once, when a frame is first in it; one with no DWARF file, such as the
     * system's libraries, is remembered so it isn't looked for again. */
    object objects[MAX_OBJECTS];
    const void* headers[MAX_OBJECTS];
    int usable[MAX_OBJECTS];
    int count = 0;
    if (!getcwd(f->cwd, sizeof f->cwd)) f->cwd[0] = 0;
    for (int i = 0; i < n; i++) {
        /* A return address is just past its call, which may end the call's line. */
        uintptr_t ret = (uintptr_t)frames[i] - 1;
        Dl_info info;
        if (!dladdr((const void*)ret, &info) || !info.dli_fbase || !info.dli_fname) continue;
        int k = 0;
        for (; k < count; k++) {
            if (headers[k] == info.dli_fbase) break;
        }
        if (k == count) {
            if (count == MAX_OBJECTS) continue;
            headers[count] = info.dli_fbase;
            usable[count] = open_image(&objects[count], info.dli_fbase, info.dli_fname);
            count++;
        }
        if (trace_debug()) {
            fprintf(stderr, "trace: frame %d %p in %s, %s\n", i, frames[i], info.dli_fname,
                    usable[k] ? "with its DWARF" : "with no DWARF found");
        }
        if (!usable[k]) continue;
        /* dladdr's name is the nearest symbol, without macOS's leading '_'. */
        dwarf_frames(f, &objects[k].d, (cx_u64)(ret - objects[k].base), info.dli_sname, 1);
    }
    for (int k = 0; k < count; k++) {
        if (objects[k].image) munmap((void*)objects[k].image, objects[k].size);
    }
    pthread_mutex_unlock(&trace_lock);
}

#else

/* Maps the ELF file at `path`, and finds its symbols and line tables. */
static int open_object(object* o, const char* path, uintptr_t base) {
    memset(o, 0, sizeof *o);
    o->base = base;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(Elf64_Ehdr)) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    const cx_u8* image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (image == MAP_FAILED) return 0;
    o->image = image;
    o->size = size;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)image;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff > size ||
        (size - eh->e_shoff) / sizeof(Elf64_Shdr) < eh->e_shnum || eh->e_shstrndx >= eh->e_shnum) {
        return 0;
    }
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(image + eh->e_shoff);
    span names = {NULL, 0};
    const Elf64_Shdr* sn = &sh[eh->e_shstrndx];
    if (sn->sh_offset > size || sn->sh_size > size - sn->sh_offset) return 0;
    names.p = image + sn->sh_offset;
    names.n = sn->sh_size;
    dwarf* d = &o->d;
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_NOBITS || sh[i].sh_offset > size || sh[i].sh_size > size - sh[i].sh_offset) continue;
        if (sh[i].sh_flags & SHF_COMPRESSED) continue;
        span sec = {image + sh[i].sh_offset, sh[i].sh_size};
        const char* name = string_at(names, sh[i].sh_name);
        if (sh[i].sh_type == SHT_SYMTAB) {
            o->symtab = sec;
            if (sh[i].sh_link < eh->e_shnum) {
                const Elf64_Shdr* l = &sh[sh[i].sh_link];
                if (l->sh_offset <= size && l->sh_size <= size - l->sh_offset) {
                    o->strtab.p = image + l->sh_offset;
                    o->strtab.n = l->sh_size;
                }
            }
        } else if (strcmp(name, ".debug_line") == 0) {
            d->line = sec;
        } else if (strcmp(name, ".debug_line_str") == 0) {
            d->line_str = sec;
        } else if (strcmp(name, ".debug_str") == 0) {
            d->str = sec;
        } else if (strcmp(name, ".debug_info") == 0) {
            d->info = sec;
        } else if (strcmp(name, ".debug_abbrev") == 0) {
            d->abbrev = sec;
        } else if (strcmp(name, ".debug_addr") == 0) {
            d->addr = sec;
        } else if (strcmp(name, ".debug_str_offsets") == 0) {
            d->str_offsets = sec;
        } else if (strcmp(name, ".debug_rnglists") == 0) {
            d->rnglists = sec;
        }
    }
    return o->symtab.p && d->line.p;
}

/* Which loaded object an address is in: its base and its file. */
typedef struct {
    uintptr_t at;
    uintptr_t base;
    char path[4096];
    int found;
} containing;

static int find_object(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    containing* c = (containing*)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t lo = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
        if (c->at >= lo && c->at - lo < (uintptr_t)ph->p_memsz) {
            c->base = (uintptr_t)info->dlpi_addr;
            /* The program itself has no name here. */
            const char* name = info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "/proc/self/exe";
            snprintf(c->path, sizeof c->path, "%s", name);
            c->found = 1;
            return 1;
        }
    }
    return 0;
}

static void name_frames(found* f, void* const* frames, int n) {
    pthread_mutex_lock(&trace_lock);
    /* Each object is opened once, when a frame is first in it; one without line tables, such as
     * the C library, is remembered so it isn't opened again. */
    object objects[MAX_OBJECTS];
    int usable[MAX_OBJECTS];
    int count = 0;
    if (!getcwd(f->cwd, sizeof f->cwd)) f->cwd[0] = 0;
    for (int i = 0; i < n; i++) {
        /* A return address is just past its call, which may end the call's line. */
        uintptr_t ret = (uintptr_t)frames[i] - 1;
        containing c;
        c.at = ret;
        c.found = 0;
        dl_iterate_phdr(find_object, &c);
        if (!c.found) continue;
        object* o = NULL;
        int k = 0;
        for (; k < count; k++) {
            if (objects[k].base == c.base) break;
        }
        if (k == count) {
            if (count == MAX_OBJECTS) continue;
            usable[count] = open_object(&objects[count], c.path, c.base);
            count++;
        }
        if (!usable[k]) continue;
        o = &objects[k];
        cx_u64 at = (cx_u64)(ret - o->base);
        const Elf64_Sym* syms = (const Elf64_Sym*)o->symtab.p;
        size_t sym_count = o->symtab.n / sizeof(Elf64_Sym);
        const char* name = NULL;
        for (size_t s = 0; s < sym_count; s++) {
            if (ELF64_ST_TYPE(syms[s].st_info) != STT_FUNC || syms[s].st_shndx == SHN_UNDEF) continue;
            if (syms[s].st_value <= at && at - syms[s].st_value < syms[s].st_size) {
                name = string_at(o->strtab, syms[s].st_name);
                break;
            }
        }
        if (!name || !*name) continue;
        dwarf_frames(f, &o->d, at, name, 0);
    }
    for (int k = 0; k < count; k++) {
        if (objects[k].image) munmap((void*)objects[k].image, objects[k].size);
    }
    pthread_mutex_unlock(&trace_lock);
}

#endif /* __APPLE__ */

#endif
