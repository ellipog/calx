/* Starting other programs, behind std.process's `Command` and `Child`.
 *
 * A child is a `proc`: the process, and up to three streams to it, its input, its output and its
 * errors, each a pipe whose other end the child holds. What a child writes is read into a buffer
 * here as it comes, whatever the program is waiting for, so a child never stops on a full pipe
 * while this one waits for something else, such as its other stream. One loop, `pump`, drives
 * every stream of every child being waited for, and the children `output_all` runs at once share
 * it. On Windows the pipes are named pipes opened for overlapped I/O, whose reads and writes
 * complete on an I/O completion port, and a thread pool wait posts each child's exit to the same
 * port, as libuv does. On Linux they're nonblocking pipes, and `poll` waits on them and on a pidfd
 * that says when the child exits; on macOS that's a kqueue watching for the exit, which `poll`
 * waits on as it does a pidfd.
 *
 * A child is finished once it has exited and what it wrote has been read: after it exits, each
 * pipe is read until it's empty, not until every end of it is closed, since a program it started
 * may hold the pipe open long after, as a server started in the background would.
 *
 * The program to run is found here, the same way on each system: a name with no path in it is
 * looked for on PATH, and only there, and a relative path is relative to this program's current
 * folder, not the child's. On Windows a name is tried with `.exe`, `.com`, `.cmd` and `.bat`, and
 * a batch file runs through cmd.exe with its arguments quoted so cmd.exe passes them on as they
 * are, as Rust's `Command` does (CVE-2024-24576).
 *
 * A program that ends without meaning to, by a panic, takes its children with it, so a bug
 * can't leave them running, or, as a test that started itself without telling the copy it was a
 * copy once did, leave thousands. On Windows every child goes into a job that's killed when this
 * program ends any way but as it means to, crashing or killed too, and with the children every
 * program they started; on Linux a panic kills the children this program started. One that ends
 * as it means to, by returning from `main` or calling `exit`, leaves them running.
 *
 * This file is compiled into a program only when it starts other programs. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h> /* K32GetProcessMemoryInfo, which kernel32 has */
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/event.h>
#else
#include <sched.h>
#include <sys/syscall.h>
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#endif
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "calx_rt.h"

CX_COLD void cx_panic_text(const char* at, const char* message);

/* In calx_rt.c: called as the program ends, with whether it's ending as it means to. */
extern void (*cx_children_at_exit)(int normal);

/* Where a stream goes, as std.process numbers `Stdio`. */
enum { IO_INHERIT = 0, IO_PIPE = 1, IO_NULL = 2, IO_STDOUT = 3 };
/* Why a child didn't start, as std.process numbers `ProcessError`. */
enum { E_NOT_FOUND = 1, E_DENIED = 2, E_BAD_DIR = 3, E_BAD_ARG = 4, E_CLOSED = 5, E_OS = 6 };
/* `cx_proc_state`'s bits. */
enum { ST_EXITED = 1, ST_KILLED = 2, ST_TIMED_OUT = 4, ST_FINISHED = 8, ST_HAS_CODE = 16 };

/* Room made for each read, so a large output takes few. */
#define READ_CHUNK 65536

static _Thread_local cx_u32 err_kind;
static _Thread_local cx_i32 err_code;
/* `cx_proc_which`'s answer, which std.process copies out. */
static _Thread_local char* result;
static _Thread_local size_t result_len, result_cap;

static void fail(cx_u32 kind, cx_i32 code) {
    err_kind = kind;
    err_code = code;
}

static CX_COLD void out_of_memory(void) { cx_panic_text("<memory>", "out of memory"); }

static void* grow(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) out_of_memory();
    return q;
}

static void set_result(const char* s, size_t n) {
    if (n > result_cap) {
        result = (char*)grow(result, n);
        result_cap = n;
    }
    memcpy(result, s, n);
    result_len = n;
}

/* Bytes waiting in a stream: `start..len` of `p`. */
typedef struct {
    cx_u8* p;
    size_t start, len, cap;
} bytes;

/* Makes room for `want` more bytes after `len`, moving what's waiting to the front first. */
static void make_room(bytes* b, size_t want, size_t* scan) {
    if (b->start && b->cap - b->len < want) {
        size_t keep = b->len - b->start;
        memmove(b->p, b->p + b->start, keep);
        if (scan) *scan = *scan > b->start ? *scan - b->start : 0;
        b->len = keep;
        b->start = 0;
    }
    if (b->cap - b->len < want) {
        size_t c = b->cap ? b->cap * 2 : READ_CHUNK;
        while (c - b->len < want) c *= 2;
        b->p = (cx_u8*)grow(b->p, c);
        b->cap = c;
    }
}

static void bytes_add(bytes* b, const cx_u8* s, size_t n) {
    if (!n) return;
    make_room(b, n, NULL);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

typedef struct proc proc;

/* A stream to a child: 0 its input, 1 its output, 2 its errors. */
typedef struct {
    bytes b;        /* output: what came and wasn't taken; input: what's still to write */
    size_t scan;    /* output: how far `b` has been searched for a line's end */
    int open;       /* this end is open, so more may come or go */
    int draining;   /* output: the child has exited, so what's in the pipe is all that's left */
    int broken;     /* input: the child stopped reading before all of it was written */
    int index;
    proc* owner;
#ifdef _WIN32
    HANDLE h;
    OVERLAPPED ov;
    int pending;    /* a read or a write is in flight, into or out of `b` */
    bytes more;     /* input: what to write once the write in flight is done */
#else
    int fd;
#endif
} stream;

/* The children that wait together: on Windows, those whose I/O completes on one port. */
typedef struct {
    int refs;
#ifdef _WIN32
    HANDLE port;
#endif
} group;

struct proc {
    stream s[3];
    int close_input;    /* close the input once what's queued is written */
    int exited, killed, timed_out, has_code;
    cx_i32 code, signal;
    cx_u64 deadline;    /* when its timeout runs out, on cx_clock_ns's clock, or 0 */
    cx_u64 cpu_ns, peak_memory;   /* what it used in all, recorded as it exited */
    cx_u64 started_ns, ended_ns;  /* on cx_clock_ns's clock: just before it started, and once it was seen to exit */
    cx_u32 pid;
    group* g;
#ifdef _WIN32
    HANDLE process, wait;
    volatile LONG exit_posted;
    int exit_seen;
#else
    int pidfd;
    unsigned backoff;   /* without a pidfd, how many ms to wait before looking again */
    proc *live_prev, *live_next;   /* in `live` while it hasn't been seen to exit */
    int listed;
#endif
};

static proc* P(cx_i64 h) { return (proc*)(intptr_t)h; }

static group* group_new(void) {
    group* g = (group*)calloc(1, sizeof *g);
    if (!g) out_of_memory();
    g->refs = 1;
#ifdef _WIN32
    g->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!g->port) {
        free(g);
        return NULL;
    }
#endif
    return g;
}

static void group_release(group* g) {
    if (--g->refs) return;
#ifdef _WIN32
    CloseHandle(g->port);
#endif
    free(g);
}

/* --- The spec std.process sends ----------------------------------------------------------- */

typedef struct {
    const cx_u8* p;
    size_t n;
} span;

typedef struct {
    int set;
    span name, value;
} env_change;

/* A command, as std.process writes it: the three streams' `Stdio`s and whether the environment
 * starts empty, a byte each; the timeout in ms as a u64; the program; the arguments, a u32 count
 * of them; whether there's a folder, a byte, and the folder; then a u32 count of changes to the
 * environment, each a byte that's 1 to set the variable and 0 to remove it, its name, and its
 * value if it's set; then the processors it may run on, a u32 count and a u32 each, none for any;
 * a byte that's 1 to take it out of power throttling; and a byte that's 1 to start it from a
 * helper, so its peak memory is its own. Integers are little-endian, and each text is a u32
 * length and its bytes. */
typedef struct {
    cx_u32 io[3];
    int clear_env;
    cx_u64 timeout_ms;
    span program;
    span* args;
    size_t argc;
    int has_dir;
    span dir;
    env_change* env;
    size_t envc;
    cx_u32* cpus;
    size_t ncpus;
    int full_speed;
    int measured;
} spec;

typedef struct {
    const cx_u8* p;
    size_t n, at;
    int bad;
} reader;

static cx_u64 rd_int(reader* r, int size) {
    if (r->n - r->at < (size_t)size) {
        r->bad = 1;
        return 0;
    }
    cx_u64 v = 0;
    for (int i = 0; i < size; i++) v |= (cx_u64)r->p[r->at + i] << (8 * i);
    r->at += (size_t)size;
    return v;
}

static span rd_span(reader* r) {
    span s = {NULL, 0};
    size_t n = (size_t)rd_int(r, 4);
    if (r->bad || r->n - r->at < n) {
        r->bad = 1;
        return s;
    }
    s.p = r->p + r->at;
    s.n = n;
    r->at += n;
    return s;
}

static void spec_free(spec* sp) {
    free(sp->args);
    free(sp->env);
    free(sp->cpus);
}

static int spec_read(spec* sp, const cx_u8* p, size_t n) {
    memset(sp, 0, sizeof *sp);
    reader r = {p, n, 0, 0};
    for (int i = 0; i < 3; i++) sp->io[i] = (cx_u32)rd_int(&r, 1);
    sp->clear_env = (int)rd_int(&r, 1);
    sp->timeout_ms = rd_int(&r, 8);
    sp->program = rd_span(&r);
    sp->argc = (size_t)rd_int(&r, 4);
    if (r.bad || sp->argc > n) return 0;
    sp->args = (span*)grow(NULL, sp->argc * sizeof(span));
    for (size_t i = 0; i < sp->argc; i++) sp->args[i] = rd_span(&r);
    sp->has_dir = (int)rd_int(&r, 1);
    if (sp->has_dir) sp->dir = rd_span(&r);
    sp->envc = (size_t)rd_int(&r, 4);
    if (r.bad || sp->envc > n) return 0;
    sp->env = (env_change*)grow(NULL, sp->envc * sizeof(env_change));
    for (size_t i = 0; i < sp->envc; i++) {
        sp->env[i].set = (int)rd_int(&r, 1);
        sp->env[i].name = rd_span(&r);
        sp->env[i].value.p = NULL;
        sp->env[i].value.n = 0;
        if (sp->env[i].set) sp->env[i].value = rd_span(&r);
    }
    sp->ncpus = (size_t)rd_int(&r, 4);
    if (r.bad || sp->ncpus > n) return 0;
    sp->cpus = (cx_u32*)grow(NULL, sp->ncpus * sizeof(cx_u32));
    for (size_t i = 0; i < sp->ncpus; i++) sp->cpus[i] = (cx_u32)rd_int(&r, 4);
    sp->full_speed = (int)rd_int(&r, 1);
    sp->measured = (int)rd_int(&r, 1);
    return !r.bad;
}

/* --- Windows ------------------------------------------------------------------------------- */
#ifdef _WIN32

typedef struct {
    wchar_t* p;
    size_t len, cap;
} wbuf;

static void w_reserve(wbuf* b, size_t more) {
    if (b->cap - b->len < more + 1) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c - b->len < more + 1) c *= 2;
        b->p = (wchar_t*)grow(b->p, c * sizeof(wchar_t));
        b->cap = c;
    }
}

static void w_push(wbuf* b, wchar_t c) {
    w_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void w_add(wbuf* b, const wchar_t* s, size_t n) {
    w_reserve(b, n);
    memcpy(b->p + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->p[b->len] = 0;
}

/* Adds UTF-8 text, as UTF-16. Calx's text is always valid UTF-8. */
static void w_utf8(wbuf* b, span s) {
    w_reserve(b, 0);
    b->p[b->len] = 0;
    if (s.n == 0 || s.n > 0x7fffffff) return;
    int k = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.p, (int)s.n, NULL, 0);
    w_reserve(b, (size_t)k);
    MultiByteToWideChar(CP_UTF8, 0, (const char*)s.p, (int)s.n, b->p + b->len, k);
    b->len += (size_t)k;
    b->p[b->len] = 0;
}

static void w_free(wbuf* b) { free(b->p); }

static int is_file_w(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* What a name is tried with, in this order, when it has no extension or nothing has its own. */
static const wchar_t* const EXTS[] = {L".exe", L".com", L".cmd", L".bat"};

/* Whether the path in `b`, or it with one of EXTS, is a file; leaves the one found in `b`. */
static int try_file(wbuf* b, int dot) {
    size_t n = b->len;
    if (dot && is_file_w(b->p)) return 1;
    for (int i = 0; i < 4; i++) {
        b->len = n;
        w_add(b, EXTS[i], 4);
        if (is_file_w(b->p)) return 1;
    }
    b->len = n;
    b->p[n] = 0;
    return 0;
}

/* The program `name` names, as a full path in `out`: a path is made whole from this program's
 * current folder, and a name is looked for in each folder on `path`. */
static int resolve_w(const wchar_t* name, size_t n, const wchar_t* path, wbuf* out) {
    int sep = 0, dot = 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = name[i];
        if (c == L'\\' || c == L'/' || c == L':') {
            sep = 1;
            dot = 0;
        } else if (c == L'.') {
            dot = 1;
        }
    }
    out->len = 0;
    if (sep) {
        DWORD k = GetFullPathNameW(name, 0, NULL, NULL);
        if (!k) return 0;
        w_reserve(out, k + 8);
        k = GetFullPathNameW(name, k, out->p, NULL);
        out->len = k;
        return try_file(out, dot);
    }
    if (!path) return 0;
    const wchar_t* p = path;
    while (*p) {
        const wchar_t* e = p;
        while (*e && *e != L';') e++;
        const wchar_t* a = p;
        const wchar_t* z = e;
        if (z > a && *a == L'"') {
            a++;
            if (z > a && z[-1] == L'"') z--;
        }
        if (z > a) {
            out->len = 0;
            w_add(out, a, (size_t)(z - a));
            if (out->p[out->len - 1] != L'\\' && out->p[out->len - 1] != L'/') w_push(out, L'\\');
            w_add(out, name, n);
            if (try_file(out, dot)) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int ends_with_ci(const wbuf* b, const wchar_t* ext) {
    size_t n = wcslen(ext);
    return b->len >= n && CompareStringOrdinal(b->p + b->len - n, (int)n, ext, (int)n, TRUE) == CSTR_EQUAL;
}

/* An argument as the C runtime's parser reads it back: quoted if it's empty or has a space, a
 * tab or a quote, a quote escaped with a backslash, and the backslashes before a quote doubled. */
static void quote_arg(wbuf* b, const wchar_t* a, size_t n) {
    int quote = n == 0;
    for (size_t i = 0; i < n && !quote; i++) quote = a[i] == L' ' || a[i] == L'\t' || a[i] == L'\n' || a[i] == L'\v' || a[i] == L'"';
    if (!quote) {
        w_add(b, a, n);
        return;
    }
    w_push(b, L'"');
    size_t slashes = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] == L'\\') {
            slashes++;
            continue;
        }
        size_t k = a[i] == L'"' ? slashes * 2 + 1 : slashes;
        while (k--) w_push(b, L'\\');
        w_push(b, a[i]);
        slashes = 0;
    }
    for (size_t k = slashes * 2; k; k--) w_push(b, L'\\');
    w_push(b, L'"');
}

/* An argument to a batch file, which cmd.exe reads by its own rules before the batch file sees
 * it: quoted unless it's only letters, digits and marks cmd.exe leaves alone, a quote doubled,
 * and each `%` written as `%%cd:~,%`, which expands to the `%` alone and keeps cmd.exe from
 * reading a variable's name after it. A line break would end the command, so it can't be passed. */
static int quote_bat_arg(wbuf* b, const wchar_t* a, size_t n) {
    int quote = n == 0 || a[n - 1] == L'\\';
    for (size_t i = 0; i < n; i++) {
        wchar_t c = a[i];
        if (c == L'\r' || c == L'\n') return 0;
        if (c < 0x80) {
            int plain = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || wcschr(L"#$*+-./:?@\\_", c) != NULL;
            if (!plain || c == 0) quote = 1;
        } else if (c <= 0x9f) {
            quote = 1;
        }
    }
    if (quote) w_push(b, L'"');
    size_t slashes = 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = a[i];
        if (c == L'\\') {
            slashes++;
        } else {
            if (c == L'"') {
                for (size_t k = slashes; k; k--) w_push(b, L'\\');
                w_push(b, L'"');
            } else if (c == L'%') {
                w_add(b, L"%%cd:~,", 7);
            }
            slashes = 0;
        }
        w_push(b, c);
    }
    if (quote) {
        for (size_t k = slashes; k; k--) w_push(b, L'\\');
        w_push(b, L'"');
    }
    return 1;
}

/* The name part of an environment entry, `NAME=value`: up to the first `=` after its first
 * character, since the entries Windows keeps for each drive's folder start with one. */
static size_t env_name_len(const wchar_t* e) {
    const wchar_t* eq = e[0] ? wcschr(e + 1, L'=') : NULL;
    return eq ? (size_t)(eq - e) : wcslen(e);
}

static int env_cmp(const void* x, const void* y) {
    const wchar_t* a = *(const wchar_t* const*)x;
    const wchar_t* b = *(const wchar_t* const*)y;
    return CompareStringOrdinal(a, (int)env_name_len(a), b, (int)env_name_len(b), TRUE) - CSTR_EQUAL;
}

/* The child's environment block, sorted by name as Windows expects, and its PATH in `path` if
 * the changes set one. */
static void env_block_w(const spec* sp, wbuf* out, wbuf* path, int* path_set) {
    wchar_t** list = NULL;
    size_t n = 0, cap = 0;
    wchar_t* all = NULL;
    if (!sp->clear_env) {
        all = GetEnvironmentStringsW();
        for (wchar_t* e = all; e && *e; e += wcslen(e) + 1) {
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                list = (wchar_t**)grow(list, cap * sizeof *list);
            }
            list[n++] = e;
        }
    }
    wbuf made = {0};   /* the entries the changes add, each ending with a NUL */
    size_t* starts = NULL;
    size_t made_n = 0;
    for (size_t c = 0; c < sp->envc; c++) {
        wbuf name = {0};
        w_utf8(&name, sp->env[c].name);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            size_t len = env_name_len(list[i]);
            if (!(len == name.len && CompareStringOrdinal(list[i], (int)len, name.p, (int)name.len, TRUE) == CSTR_EQUAL)) list[k++] = list[i];
        }
        n = k;
        /* Entries the changes made are found again by their place in `made`, since it moves. */
        size_t m = 0;
        for (size_t i = 0; i < made_n; i++) {
            const wchar_t* e = made.p + starts[i];
            size_t len = env_name_len(e);
            if (!(len == name.len && CompareStringOrdinal(e, (int)len, name.p, (int)name.len, TRUE) == CSTR_EQUAL)) starts[m++] = starts[i];
        }
        made_n = m;
        int is_path = name.len == 4 && CompareStringOrdinal(name.p, 4, L"PATH", 4, TRUE) == CSTR_EQUAL;
        if (sp->env[c].set) {
            starts = (size_t*)grow(starts, (made_n + 1) * sizeof *starts);
            starts[made_n++] = made.len;
            w_add(&made, name.p, name.len);
            w_push(&made, L'=');
            w_utf8(&made, sp->env[c].value);
            w_push(&made, 0);
            if (is_path) {
                path->len = 0;
                w_utf8(path, sp->env[c].value);
                *path_set = 1;
            }
        } else if (is_path) {
            *path_set = 0;
        }
        w_free(&name);
    }
    list = (wchar_t**)grow(list, (n + made_n) * sizeof *list);
    for (size_t i = 0; i < made_n; i++) list[n++] = made.p + starts[i];
    qsort(list, n, sizeof *list, env_cmp);
    out->len = 0;
    for (size_t i = 0; i < n; i++) {
        w_add(out, list[i], wcslen(list[i]));
        w_push(out, 0);
    }
    if (n == 0) w_push(out, 0);
    w_push(out, 0);
    if (all) FreeEnvironmentStringsW(all);
    free(list);
    free(starts);
    w_free(&made);
}

static wchar_t* path_var_w(wbuf* b) {
    DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (!n) return NULL;
    b->len = 0;
    w_reserve(b, n);
    b->len = GetEnvironmentVariableW(L"PATH", b->p, n);
    b->p[b->len] = 0;
    return b->p;
}

/* A name no other pipe has: this process, a count, and the clock. */
static void pipe_name(wchar_t* out) {
    static volatile LONG counter;
    unsigned long long parts[3] = {GetCurrentProcessId(), (unsigned long long)InterlockedIncrement(&counter), cx_clock_ns()};
    const wchar_t* prefix = L"\\\\.\\pipe\\calx-";
    size_t k = 0;
    while (prefix[k]) {
        out[k] = prefix[k];
        k++;
    }
    for (int i = 0; i < 3; i++) {
        if (i) out[k++] = L'-';
        wchar_t digits[16];
        int m = 0;
        unsigned long long v = parts[i];
        do {
            digits[m++] = L"0123456789abcdef"[v & 15];
            v >>= 4;
        } while (v);
        while (m) out[k++] = digits[--m];
    }
    out[k] = 0;
}

/* A pipe whose end here is overlapped, and whose other end the child inherits: `reads` says
 * whether this end reads. */
static int make_pipe(HANDLE* ours, HANDLE* theirs, int reads) {
    for (int tries = 0; tries < 16; tries++) {
        wchar_t name[80];
        pipe_name(name);
        DWORD mode = (reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        HANDLE a = CreateNamedPipeW(name, mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, READ_CHUNK,
                                    READ_CHUNK, 0, NULL);
        if (a == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY) continue;
            return 0;
        }
        SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        DWORD access = reads ? GENERIC_WRITE | FILE_READ_ATTRIBUTES : GENERIC_READ | FILE_WRITE_ATTRIBUTES;
        HANDLE b = CreateFileW(name, access, 0, &sa, OPEN_EXISTING, 0, NULL);
        if (b == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            CloseHandle(a);
            SetLastError(e);
            return 0;
        }
        *ours = a;
        *theirs = b;
        return 1;
    }
    return 0;
}

/* An inheritable copy of this program's own stream, or NULL if it has none. */
static HANDLE dup_std(DWORD which) {
    HANDLE h = GetStdHandle(which);
    HANDLE d = NULL;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return d;
}

static HANDLE open_null(int reads) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileW(L"NUL", reads ? GENERIC_READ : GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : h;
}

/* Takes the process `h` out of power throttling, EcoQoS, so Windows doesn't move it to the
 * efficiency cores, or run them slower, to save power: ProcessPowerThrottling, 4, at version 1,
 * the execution speed's control, off. SetProcessInformation is Windows 8's, which headers built
 * for older Windows don't declare, so it's looked up, once. */
typedef BOOL(WINAPI* set_info_fn)(HANDLE, int, LPVOID, DWORD);

static void full_speed_win(HANDLE h) {
    static set_info_fn set_info;
    static volatile LONG looked;
    if (!InterlockedCompareExchange(&looked, 0, 0)) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        set_info = k ? (set_info_fn)(void*)GetProcAddress(k, "SetProcessInformation") : NULL;
        InterlockedExchange(&looked, 1);
    }
    struct {
        ULONG version, control, state;
    } speed = {1, 1, 0};
    if (set_info) set_info(h, 4, &speed, sizeof speed);
}

static void fail_win(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        fail(E_NOT_FOUND, (cx_i32)e);
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        fail(E_DENIED, (cx_i32)e);
        break;
    case ERROR_DIRECTORY:
        fail(E_BAD_DIR, (cx_i32)e);
        break;
    default:
        fail(E_OS, (cx_i32)e);
    }
}

#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

/* The job every child goes into as it starts, with the programs it starts in turn. If this
 * program ends without meaning to, by a panic, a crash, or being killed, the job closes and kills
 * all of them, so a program that went wrong leaves nothing running, however deep the programs it
 * started go. One that ends as it means to lets them go first, and they run on, as children do
 * without a job. A child may still leave the job, as a program that starts something in the
 * background does. */
static HANDLE children_job;
static INIT_ONCE children_once = INIT_ONCE_STATIC_INIT;

static BOOL job_limits(DWORD flags) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    memset(&info, 0, sizeof info);
    info.BasicLimitInformation.LimitFlags = flags;
    return SetInformationJobObject(children_job, JobObjectExtendedLimitInformation, &info, sizeof info);
}

static void children_at_exit(int normal) {
    if (normal) job_limits(JOB_OBJECT_LIMIT_BREAKAWAY_OK);
    else TerminateJobObject(children_job, 1);
}

static BOOL CALLBACK make_job(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    children_job = CreateJobObjectW(NULL, NULL);
    if (children_job && !job_limits(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK)) {
        CloseHandle(children_job);
        children_job = NULL;
    }
    if (children_job) __atomic_store_n(&cx_children_at_exit, children_at_exit, __ATOMIC_RELEASE);
    return TRUE;
}

/* The handles the child inherits and the job it goes into, for CreateProcess. */
static LPPROC_THREAD_ATTRIBUTE_LIST make_attrs(HANDLE* list, DWORD count, int job) {
    DWORD n = (count ? 1 : 0) + (job ? 1 : 0);
    if (!n) return NULL;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, n, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)grow(NULL, size);
    if (!InitializeProcThreadAttributeList(attrs, n, 0, &size)) {
        free(attrs);
        return NULL;
    }
    if ((count && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, count * sizeof(HANDLE), NULL, NULL)) ||
        (job && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &children_job, sizeof(HANDLE), NULL, NULL))) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
        return NULL;
    }
    return attrs;
}

static void free_attrs(LPPROC_THREAD_ATTRIBUTE_LIST attrs) {
    if (!attrs) return;
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
}

static void close_stream(stream* s) {
    if (!s->open) return;
    s->open = 0;
    if (s->pending) {
        CancelIoEx(s->h, &s->ov);   /* its completion still comes, and closes it */
    } else {
        CloseHandle(s->h);
        s->h = NULL;
    }
}

static int queued(const stream* s) { return s->open && (s->pending || s->b.start < s->b.len || s->more.len); }

static void start_read(stream* s) {
    if (!s->open || s->pending) return;
    if (s->draining) {
        DWORD avail = 0;
        if (!PeekNamedPipe(s->h, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            close_stream(s);
            return;
        }
    }
    make_room(&s->b, READ_CHUNK, &s->scan);
    size_t n = s->b.cap - s->b.len;
    if (n > (1u << 30)) n = 1u << 30;
    memset(&s->ov, 0, sizeof s->ov);
    /* It completes on the port even when it succeeds at once. */
    if (!ReadFile(s->h, s->b.p + s->b.len, (DWORD)n, NULL, &s->ov) && GetLastError() != ERROR_IO_PENDING) {
        close_stream(s);
        return;
    }
    s->pending = 1;
}

static void start_write(stream* s) {
    if (!s->open || s->pending) return;
    if (s->b.start == s->b.len) {
        s->b.start = s->b.len = 0;
        if (!s->more.len) {
            if (s->owner->close_input) close_stream(s);
            return;
        }
        bytes t = s->b;
        s->b = s->more;
        s->more = t;
    }
    size_t n = s->b.len - s->b.start;
    if (n > (1u << 30)) n = 1u << 30;
    memset(&s->ov, 0, sizeof s->ov);
    if (!WriteFile(s->h, s->b.p + s->b.start, (DWORD)n, NULL, &s->ov) && GetLastError() != ERROR_IO_PENDING) {
        s->broken = 1;
        close_stream(s);
        return;
    }
    s->pending = 1;
}

static void io_done(stream* s, DWORD n) {
    s->pending = 0;
    DWORD got = 0;
    DWORD err = GetOverlappedResult(s->h, &s->ov, &got, FALSE) ? 0 : GetLastError();
    if (!s->open) {
        /* Input closed with a write in flight, as when the child exits: unless that write went
         * in whole with nothing after it, the child didn't read it all (CLX-312). */
        if (s->index == 0 && (err || s->b.start + n < s->b.len || s->more.len)) s->broken = 1;
        CloseHandle(s->h);
        s->h = NULL;
        return;
    }
    if (s->index == 0) {
        if (err) {
            s->broken = 1;
            close_stream(s);
            return;
        }
        s->b.start += n;
        start_write(s);
        return;
    }
    s->b.len += n;
    if (err && err != ERROR_OPERATION_ABORTED && err != ERROR_MORE_DATA) {
        close_stream(s);   /* ERROR_BROKEN_PIPE: every writer has closed it */
        return;
    }
    start_read(s);
}

/* What the process has used: its CPU time in ns, in the kernel and out, and its working set now
 * and at its peak, in bytes. A handle to a process that has exited still says what it used. */
static void usage_os(proc* p, cx_u64* out) {
    FILETIME made, ended, kernel, user;
    out[0] = 0;
    if (GetProcessTimes(p->process, &made, &ended, &kernel, &user)) {
        cx_u64 k = ((cx_u64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        cx_u64 u = ((cx_u64)user.dwHighDateTime << 32) | user.dwLowDateTime;
        out[0] = (k + u) * 100;   /* in units of 100 ns */
    }
    PROCESS_MEMORY_COUNTERS c;
    int known = K32GetProcessMemoryInfo(p->process, &c, sizeof c) != 0;
    out[1] = known ? (cx_u64)c.WorkingSetSize : 0;
    out[2] = known ? (cx_u64)c.PeakWorkingSetSize : 0;
}

/* The child has exited: what's in its pipes is all it wrote. */
static void child_exited(proc* p) {
    if (p->exited) return;
    DWORD code = 0;
    GetExitCodeProcess(p->process, &code);
    cx_u64 used[3];
    usage_os(p, used);
    p->cpu_ns = used[0];
    p->peak_memory = used[2];
    p->ended_ns = cx_clock_ns();
    p->exited = 1;
    p->deadline = 0;
    p->code = (cx_i32)code;
    p->has_code = !p->killed;
    /* Input still waiting to go in wasn't read. A write in flight is cancelled by close_stream,
     * and its completion, which io_done takes, says whether it went in whole (CLX-312). */
    stream* in = &p->s[0];
    if (in->open && !in->pending && (in->b.start < in->b.len || in->more.len)) in->broken = 1;
    close_stream(in);
    for (int i = 1; i < 3; i++) {
        stream* s = &p->s[i];
        if (!s->open) continue;
        s->draining = 1;
        if (!s->pending) {
            start_read(s);
        } else {
            /* A read that's waiting for more would wait for a program the child started. */
            DWORD avail = 0;
            if (!PeekNamedPipe(s->h, NULL, 0, NULL, &avail, NULL) || avail == 0) CancelIoEx(s->h, &s->ov);
        }
    }
}

static VOID CALLBACK exit_callback(PVOID ctx, BOOLEAN timer) {
    (void)timer;
    proc* p = (proc*)ctx;
    InterlockedExchange(&p->exit_posted, 1);
    PostQueuedCompletionStatus(p->g->port, 0, (ULONG_PTR)p, NULL);
}

static void kill_proc(proc* p) {
    if (!p->exited && TerminateProcess(p->process, 1)) p->killed = 1;
}

/* Waits until something happens to the children `ps`, which share a group, or until `until`
 * (0 for no limit), and handles all that happened. A child whose timeout runs out is killed. */
static void pump(proc** ps, size_t n, cx_u64 until) {
    cx_u64 now = cx_clock_ns();
    cx_u64 wake = until ? until : UINT64_MAX;
    for (size_t i = 0; i < n; i++) {
        proc* p = ps[i];
        if (p->deadline && !p->exited) {
            if (now >= p->deadline) {
                p->deadline = 0;
                kill_proc(p);
                p->timed_out = p->killed;
            } else if (p->deadline < wake) {
                wake = p->deadline;
            }
        }
    }
    DWORD ms = INFINITE;
    if (wake != UINT64_MAX) {
        cx_u64 left = wake > now ? (wake - now + 999999) / 1000000 : 0;
        ms = left >= INFINITE ? INFINITE - 1 : (DWORD)left;
    }
    /* A child waited for alone with nothing in flight has only its exit to wait for, which its
     * handle says sooner than the thread pool's wait, a thread away, would. */
    proc* p = ps[0];
    if (n == 1 && !p->exited && !p->s[0].pending && !p->s[1].pending && !p->s[2].pending && !p->s[1].open && !p->s[2].open &&
        !queued(&p->s[0])) {
        if (WaitForSingleObject(p->process, ms) == WAIT_OBJECT_0) child_exited(p);
        return;
    }
    OVERLAPPED_ENTRY e[32];
    ULONG got = 0;
    if (!GetQueuedCompletionStatusEx(ps[0]->g->port, e, 32, &got, ms, FALSE)) return;
    for (ULONG k = 0; k < got; k++) {
        if (!e[k].lpOverlapped) {
            proc* q = (proc*)e[k].lpCompletionKey;
            q->exit_seen = 1;
            child_exited(q);
        } else {
            stream* s = CONTAINING_RECORD(e[k].lpOverlapped, stream, ov);
            io_done(s, e[k].dwNumberOfBytesTransferred);
        }
    }
}

/* Starts the child. The program is looked for on the child's PATH if the changes set one, and on
 * this program's if not. */
static int spawn_os(proc* p, const spec* sp, const cx_u8* input, size_t input_n) {
    int ok = 0;
    wbuf name = {0}, path = {0}, exe = {0}, line = {0}, env = {0}, dir = {0}, arg = {0};
    int path_set = 0;
    HANDLE child[3] = {NULL, NULL, NULL};
    HANDLE ours[3] = {NULL, NULL, NULL};
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    int changes = sp->clear_env || sp->envc;
    if (sp->has_dir) {
        wbuf rel = {0};
        w_utf8(&rel, sp->dir);
        DWORD k = GetFullPathNameW(rel.p, 0, NULL, NULL);
        if (k) {
            w_reserve(&dir, k);
            dir.len = GetFullPathNameW(rel.p, k, dir.p, NULL);
        }
        w_free(&rel);
        DWORD a = k ? GetFileAttributesW(dir.p) : INVALID_FILE_ATTRIBUTES;
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            fail(E_BAD_DIR, 0);
            goto done;
        }
    }
    if (changes) env_block_w(sp, &env, &path, &path_set);
    if (!path_set) path_var_w(&path);
    w_utf8(&name, sp->program);
    if (!resolve_w(name.p, name.len, path.len ? path.p : NULL, &exe)) {
        fail(E_NOT_FOUND, ERROR_FILE_NOT_FOUND);
        goto done;
    }
    int batch = ends_with_ci(&exe, L".bat") || ends_with_ci(&exe, L".cmd");
    if (batch) {
        /* cmd.exe /c with the whole command in one more pair of quotes, which it takes off. */
        if (wcschr(exe.p, L'"') || exe.p[exe.len - 1] == L'\\') {
            fail(E_BAD_ARG, 3);
            goto done;
        }
        w_add(&line, L"cmd.exe /e:ON /v:OFF /d /c \"\"", 29);
        w_add(&line, exe.p, exe.len);
        w_push(&line, L'"');
        for (size_t i = 0; i < sp->argc; i++) {
            arg.len = 0;
            w_utf8(&arg, sp->args[i]);
            w_push(&line, L' ');
            if (!quote_bat_arg(&line, arg.p, arg.len)) {
                fail(E_BAD_ARG, 2);
                goto done;
            }
        }
        w_push(&line, L'"');
        wchar_t sys[MAX_PATH];
        UINT k = GetSystemDirectoryW(sys, MAX_PATH);
        exe.len = 0;
        w_add(&exe, sys, k);
        w_add(&exe, L"\\cmd.exe", 8);
    } else {
        quote_arg(&line, name.p, name.len);
        for (size_t i = 0; i < sp->argc; i++) {
            arg.len = 0;
            w_utf8(&arg, sp->args[i]);
            w_push(&line, L' ');
            quote_arg(&line, arg.p, arg.len);
        }
    }
    /* The processors it may run on, as a mask, each one the machine has: this processor group's,
     * up to 64. */
    DWORD_PTR pin = 0;
    if (sp->ncpus) {
        DWORD_PTR mine, machine;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &mine, &machine)) machine = 0;
        for (size_t i = 0; i < sp->ncpus; i++) {
            if (sp->cpus[i] >= 8 * sizeof(DWORD_PTR) || !((machine >> sp->cpus[i]) & 1)) {
                fail(E_BAD_ARG, 5);
                goto done;
            }
            pin |= (DWORD_PTR)1 << sp->cpus[i];
        }
    }
    static const DWORD STD[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; i++) {
        switch (sp->io[i]) {
        case IO_PIPE:
            if (!make_pipe(&ours[i], &child[i], i != 0)) {
                fail_win(GetLastError());
                goto done;
            }
            break;
        case IO_NULL:
            child[i] = open_null(i == 0);
            break;
        case IO_STDOUT:
            break;
        default:
            child[i] = dup_std(STD[i]);
        }
    }
    if (sp->io[2] == IO_STDOUT) child[2] = child[1];
    /* Only these handles go to the child, so one started on another thread at the same time
     * doesn't get this one's pipes, and keep them open. */
    HANDLE list[3];
    DWORD count = 0;
    for (int i = 0; i < 3; i++) {
        int seen = !child[i];
        for (DWORD j = 0; j < count && !seen; j++) seen = list[j] == child[i];
        if (!seen) list[count++] = child[i];
    }
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child[0];
    si.StartupInfo.hStdOutput = child[1];
    si.StartupInfo.hStdError = child[2];
    InitOnceExecuteOnce(&children_once, make_job, NULL, NULL);
    int job = children_job != NULL;
    attrs = make_attrs(list, count, job);
    if (!attrs && job) {
        job = 0;   /* a Windows without job lists, before 10's 1607 */
        attrs = make_attrs(list, count, 0);
    }
    if (!attrs && count) {
        fail_win(GetLastError());
        goto done;
    }
    si.lpAttributeList = attrs;
    if (sp->io[1] == IO_INHERIT || sp->io[2] == IO_INHERIT || (sp->io[2] == IO_STDOUT && sp->io[1] == IO_INHERIT)) cx_out_flush();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    /* A program with no console, such as one with a window of its own, would give each child a
     * console window of its own, flashing up. One whose console has no window, as one started by
     * an editor or a service often has, shares it: making each child one costs 15 ms. */
    DWORD console;
    if (GetConsoleProcessList(&console, 1) == 0) flags |= CREATE_NO_WINDOW;
    /* A child to pin, or to run at full speed, starts suspended, so it runs no instruction before. */
    if (pin || sp->full_speed) flags |= CREATE_SUSPENDED;
    PROCESS_INFORMATION pi;
    p->started_ns = cx_clock_ns();
    BOOL made = CreateProcessW(exe.p, line.p, NULL, NULL, count > 0, flags, changes ? env.p : NULL, sp->has_dir ? dir.p : NULL,
                               &si.StartupInfo, &pi);
    if (!made && job) {
        /* A job this program is in may not allow one inside it: then the child goes without. */
        DWORD e = GetLastError();
        free_attrs(attrs);
        attrs = make_attrs(list, count, 0);
        si.lpAttributeList = attrs;
        made = (attrs || !count) && CreateProcessW(exe.p, line.p, NULL, NULL, count > 0, flags, changes ? env.p : NULL,
                                                   sp->has_dir ? dir.p : NULL, &si.StartupInfo, &pi);
        if (!made) SetLastError(e);
    }
    if (!made) {
        fail_win(GetLastError());
        goto done;
    }
    if (flags & CREATE_SUSPENDED) {
        if (pin && !SetProcessAffinityMask(pi.hProcess, pin)) {
            DWORD e = GetLastError();
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            fail_win(e);
            goto done;
        }
        if (sp->full_speed) full_speed_win(pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->pid = pi.dwProcessId;
    for (int i = 0; i < 3; i++) {
        stream* s = &p->s[i];
        if (!ours[i]) continue;
        s->h = ours[i];
        ours[i] = NULL;
        s->open = 1;
        CreateIoCompletionPort(s->h, p->g->port, 0, 0);
    }
    if (!RegisterWaitForSingleObject(&p->wait, p->process, exit_callback, p, INFINITE, WT_EXECUTEONLYONCE)) {
        /* Without it the exit would never be seen, so it's as if it never started. */
        fail_win(GetLastError());
        TerminateProcess(p->process, 1);
        for (int i = 0; i < 3; i++) {
            if (p->s[i].open) CloseHandle(p->s[i].h);
            p->s[i].open = 0;
        }
        CloseHandle(p->process);
        goto done;
    }
    if (p->s[0].open) bytes_add(&p->s[0].more, input, input_n);
    start_write(&p->s[0]);
    start_read(&p->s[1]);
    start_read(&p->s[2]);
    ok = 1;
done:
    for (int i = 0; i < 3; i++) {
        if (ours[i]) CloseHandle(ours[i]);
        if (child[i] && !(i == 2 && child[2] == child[1])) CloseHandle(child[i]);
    }
    free_attrs(attrs);
    w_free(&name);
    w_free(&path);
    w_free(&exe);
    w_free(&line);
    w_free(&env);
    w_free(&dir);
    w_free(&arg);
    return ok;
}

static void free_os(proc* p) {
    if (p->wait) {
        UnregisterWaitEx(p->wait, INVALID_HANDLE_VALUE);   /* waits for the callback, if it's running */
        p->wait = NULL;
    }
    p->deadline = 0;
    for (int i = 0; i < 3; i++) close_stream(&p->s[i]);
    /* What's in flight still completes on the port, and an exit that was posted is still there:
     * both point at `p`, so they're taken before it's freed. */
    while (p->s[0].pending || p->s[1].pending || p->s[2].pending || (p->exit_posted && !p->exit_seen)) pump(&p, 1, 0);
    CloseHandle(p->process);
    free(p->s[0].more.p);
}

static cx_u32 proc_id_os(proc* p) { return p->pid; }

static int which_os(const cx_u8* name, size_t n) {
    wbuf w = {0}, path = {0}, out = {0};
    span s = {name, n};
    w_utf8(&w, s);
    int found = resolve_w(w.p, w.len, path_var_w(&path), &out);
    if (found) {
        int k = WideCharToMultiByte(CP_UTF8, 0, out.p, (int)out.len, NULL, 0, NULL, NULL);
        char* u = (char*)grow(NULL, (size_t)k);
        WideCharToMultiByte(CP_UTF8, 0, out.p, (int)out.len, u, k, NULL, NULL);
        set_result(u, (size_t)k);
        free(u);
    }
    w_free(&w);
    w_free(&path);
    w_free(&out);
    return found;
}

/* --- Linux --------------------------------------------------------------------------------- */
#else

static void close_stream(stream* s) {
    if (!s->open) return;
    s->open = 0;
    close(s->fd);
    s->fd = -1;
}

static int queued(const stream* s) { return s->open && s->b.start < s->b.len; }

/* A write that gives EPIPE where the child has closed its end, rather than SIGPIPE, which would
 * end this program: the signal is blocked around it, and taken if the write raised it. A write
 * that put some bytes in the pipe before the child closed it gives back how many, with the signal
 * raised all the same, so a short count is looked at too. */
static ssize_t write_quiet(int fd, const void* d, size_t n) {
#ifdef __APPLE__
    /* The pipe has F_SETNOSIGPIPE, so a write gives EPIPE, and macOS has no sigtimedwait. */
    return write(fd, d, n);
#else
    sigset_t pipe_set, old, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    sigpending(&pending);
    int was = sigismember(&pending, SIGPIPE);
    ssize_t r = write(fd, d, n);
    int e = errno;
    if (!was && (r < 0 ? e == EPIPE : (size_t)r < n)) {
        struct timespec zero = {0, 0};
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = e;
    return r;
#endif
}

static void write_some(stream* s) {
    bytes* b = &s->b;
    while (s->open && b->start < b->len) {
        ssize_t r = write_quiet(s->fd, b->p + b->start, b->len - b->start);
        if (r > 0) {
            b->start += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && errno == EAGAIN) {
            return;
        } else {
            s->broken = 1;
            close_stream(s);
            return;
        }
    }
    if (s->open) {
        b->start = b->len = 0;
        if (s->owner->close_input) close_stream(s);
    }
}

/* Reads what has come. Once the child has exited, what's in the pipe is the last of it. */
static void read_some(stream* s) {
    while (s->open) {
        make_room(&s->b, READ_CHUNK, &s->scan);
        size_t room = s->b.cap - s->b.len;
        ssize_t r = read(s->fd, s->b.p + s->b.len, room);
        if (r > 0) {
            s->b.len += (size_t)r;
            /* A pipe gives all it holds, so a short read emptied it. */
            if ((size_t)r < room && !s->draining) return;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && errno == EAGAIN && !s->draining) {
            return;
        } else {
            close_stream(s);   /* the end, an error, or empty after the child exited */
        }
    }
}

/* What the process has used while it runs: its CPU time in ns from /proc/<pid>/stat, and its
 * resident set now and at its peak from /proc/<pid>/status, in bytes, each 0 where /proc can't
 * say. */
#ifdef __APPLE__
/* On macOS, from proc_pid_rusage: its CPU time, counted in the machine's ticks, and its footprint
 * now and at its most, Activity Monitor's "Memory": the pages that are its own, without the shared
 * ones its resident set counts, which could put the peak below the present. */
static void usage_os(proc* p, cx_u64* out) {
    out[0] = out[1] = out[2] = 0;
    struct rusage_info_v4 ri;
    if (proc_pid_rusage((int)p->pid, RUSAGE_INFO_V4, (rusage_info_t*)&ri) != 0) return;
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) == 0 && tb.denom) {
        out[0] = (cx_u64)((unsigned __int128)(ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom);
    }
    out[1] = ri.ri_phys_footprint;
    out[2] = ri.ri_lifetime_max_phys_footprint > out[1] ? ri.ri_lifetime_max_phys_footprint : out[1];
}
#else
static void usage_os(proc* p, cx_u64* out) {
    out[0] = out[1] = out[2] = 0;
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%u/stat", (unsigned)p->pid);
    FILE* f = fopen(path, "r");
    if (f) {
        /* The name, the second field, is in parentheses and may hold spaces, so the fields are
         * counted from its last `)`: the state is the 3rd, user time the 14th, system time the 15th. */
        size_t n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[n] = 0;
        char* at = strrchr(line, ')');
        unsigned long long user, sys;
        long tick = sysconf(_SC_CLK_TCK);
        if (at && tick > 0 && sscanf(at + 1, " %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu", &user, &sys) == 2) {
            out[0] = (cx_u64)(user + sys) * (1000000000ull / (cx_u64)tick);
        }
    }
    snprintf(path, sizeof path, "/proc/%u/status", (unsigned)p->pid);
    f = fopen(path, "r");
    if (f) {
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "VmRSS: %llu", &kb) == 1) out[1] = (cx_u64)kb * 1024;
            else if (sscanf(line, "VmHWM: %llu", &kb) == 1) out[2] = (cx_u64)kb * 1024;
        }
        fclose(f);
    }
}
#endif

static void child_exited(proc* p, int status, const struct rusage* used) {
    p->ended_ns = cx_clock_ns();
    p->exited = 1;
    p->deadline = 0;
    if (used) {
        p->cpu_ns = ((cx_u64)used->ru_utime.tv_sec + (cx_u64)used->ru_stime.tv_sec) * 1000000000ull +
                    ((cx_u64)used->ru_utime.tv_usec + (cx_u64)used->ru_stime.tv_usec) * 1000ull;
#ifdef __APPLE__
        p->peak_memory = (cx_u64)used->ru_maxrss;   /* in bytes on macOS */
#else
        p->peak_memory = (cx_u64)used->ru_maxrss * 1024;   /* in KiB on Linux */
#endif
    }
    if (WIFEXITED(status)) {
        p->code = WEXITSTATUS(status);
        p->has_code = 1;
    } else if (WIFSIGNALED(status)) {
        p->signal = WTERMSIG(status);
    }
    if (p->pidfd >= 0) {
        close(p->pidfd);
        p->pidfd = -1;
    }
    /* Input still waiting to go in, its exit seen before a write's EPIPE, wasn't read (CLX-312). */
    if (queued(&p->s[0])) p->s[0].broken = 1;
    close_stream(&p->s[0]);
    for (int i = 1; i < 3; i++) {
        if (!p->s[i].open) continue;
        p->s[i].draining = 1;
        read_some(&p->s[i]);
    }
}

/* The children that haven't been seen to exit, which a panic kills, since a program that went
 * wrong shouldn't leave them running. Only these: what they started runs on, as Linux has no job
 * to hold them all, though stopping this program's process group, as a terminal's Ctrl+C or a
 * test runner does, reaches them too. */
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static proc* live;

static void children_at_exit(int normal) {
    if (normal) return;
    pthread_mutex_lock(&live_lock);
    for (proc* p = live; p; p = p->live_next) kill((pid_t)p->pid, SIGKILL);
    pthread_mutex_unlock(&live_lock);
}

static void list_live(proc* p) {
    pthread_mutex_lock(&live_lock);
    p->live_prev = NULL;
    p->live_next = live;
    if (live) live->live_prev = p;
    live = p;
    p->listed = 1;
    pthread_mutex_unlock(&live_lock);
    __atomic_store_n(&cx_children_at_exit, children_at_exit, __ATOMIC_RELEASE);
}

/* Takes it off the list, with `live_lock` held. */
static void unlist_locked(proc* p) {
    if (!p->listed) return;
    if (p->live_prev) p->live_prev->live_next = p->live_next;
    else live = p->live_next;
    if (p->live_next) p->live_next->live_prev = p->live_prev;
    p->listed = 0;
}

static void reap(proc* p) {
    int status = 0;
    struct rusage used;
    pid_t r;
    /* Off the list as it's reaped, so a panic never kills another process given its ID. wait4 says
     * what it used as well as how it ended. */
    pthread_mutex_lock(&live_lock);
    do {
        r = wait4((pid_t)p->pid, &status, WNOHANG, &used);
    } while (r < 0 && errno == EINTR);
    if (r == (pid_t)p->pid || r < 0) unlist_locked(p);
    pthread_mutex_unlock(&live_lock);
    if (r == (pid_t)p->pid) child_exited(p, status, &used);
    else if (r < 0) child_exited(p, 0x7f00, NULL);   /* reaped elsewhere: say it exited with 127 */
}

static void kill_proc(proc* p) {
    if (!p->exited && kill((pid_t)p->pid, SIGKILL) == 0) p->killed = 1;
}

typedef struct {
    proc* p;
    int what;   /* 0 to 2 a stream, 3 the pidfd */
} poll_of;

static void pump(proc** ps, size_t n, cx_u64 until) {
    cx_u64 now = cx_clock_ns();
    cx_u64 wake = until ? until : UINT64_MAX;
    struct pollfd small_fds[32];
    poll_of small_of[32];
    struct pollfd* fds = small_fds;
    poll_of* of = small_of;
    if (n * 4 > 32) {
        fds = (struct pollfd*)grow(NULL, n * 4 * sizeof *fds);
        of = (poll_of*)grow(NULL, n * 4 * sizeof *of);
    }
    size_t k = 0;
    int looking = 0;   /* a child without a pidfd, whose exit is looked for after a while */
    unsigned backoff = 1000;
    for (size_t i = 0; i < n; i++) {
        proc* p = ps[i];
        if (p->deadline && !p->exited) {
            if (now >= p->deadline) {
                p->deadline = 0;
                kill_proc(p);
                p->timed_out = p->killed;
            } else if (p->deadline < wake) {
                wake = p->deadline;
            }
        }
        if (!p->exited) {
            if (p->pidfd >= 0) {
                fds[k].fd = p->pidfd;
                fds[k].events = POLLIN;
                of[k++] = (poll_of){p, 3};
            } else {
                looking = 1;
                if (p->backoff < backoff) backoff = p->backoff;
            }
        }
        if (queued(&p->s[0])) {
            fds[k].fd = p->s[0].fd;
            fds[k].events = POLLOUT;
            of[k++] = (poll_of){p, 0};
        }
        for (int j = 1; j < 3; j++) {
            if (!p->s[j].open) continue;
            fds[k].fd = p->s[j].fd;
            fds[k].events = POLLIN;
            of[k++] = (poll_of){p, j};
        }
    }
    int ms = -1;
    if (wake != UINT64_MAX) {
        cx_u64 left = wake > now ? (wake - now + 999999) / 1000000 : 0;
        ms = left > 0x7fffffff ? 0x7fffffff : (int)left;
    }
    if (looking && (ms < 0 || (unsigned)ms > backoff)) ms = (int)backoff;
    int r = poll(fds, k, ms);
    for (size_t i = 0; r > 0 && i < k; i++) {
        if (!fds[i].revents) continue;
        proc* p = of[i].p;
        int what = of[i].what;
        if (what == 3) {
            if (!p->exited && p->pidfd == fds[i].fd) reap(p);
        } else if (p->s[what].open && p->s[what].fd == fds[i].fd) {
            if (what == 0) write_some(&p->s[0]);
            else read_some(&p->s[what]);
        }
    }
    if (looking) {
        for (size_t i = 0; i < n; i++) {
            proc* p = ps[i];
            if (p->exited || p->pidfd >= 0) continue;
            reap(p);
            if (!p->exited && p->backoff < 10) p->backoff *= 2;
        }
    }
    if (fds != small_fds) {
        free(fds);
        free(of);
    }
}

static char* cstr(span s) {
    char* c = (char*)grow(NULL, s.n + 1);
    memcpy(c, s.p, s.n);
    c[s.n] = 0;
    return c;
}

typedef struct {
    char* p;
    size_t len, cap;
} cbuf;

static void c_add(cbuf* b, const char* s, size_t n) {
    if (b->cap - b->len < n + 1) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c - b->len < n + 1) c *= 2;
        b->p = (char*)grow(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

/* The program `name` names, as a path in `out`: 1 if it's there to run, -1 if it's there but
 * can't run, 0 if nothing's there. */
static int resolve_posix(const char* name, const char* path, cbuf* out) {
    struct stat st;
    out->len = 0;
    if (strchr(name, '/')) {
        if (name[0] != '/') {
            char* cwd = getcwd(NULL, 0);
            if (!cwd) return 0;
            c_add(out, cwd, strlen(cwd));
            c_add(out, "/", 1);
            free(cwd);
        }
        c_add(out, name, strlen(name));
        if (stat(out->p, &st) != 0) return 0;
        return S_ISREG(st.st_mode) && access(out->p, X_OK) == 0 ? 1 : -1;
    }
    int seen = 0;
    for (const char* p = path ? path : ""; *p;) {
        const char* e = strchr(p, ':');
        if (!e) e = p + strlen(p);
        if (e > p) {
            out->len = 0;
            c_add(out, p, (size_t)(e - p));
            if (out->p[out->len - 1] != '/') c_add(out, "/", 1);
            c_add(out, name, strlen(name));
            if (stat(out->p, &st) == 0 && S_ISREG(st.st_mode)) {
                if (access(out->p, X_OK) == 0) return 1;
                seen = 1;
            }
        }
        p = *e ? e + 1 : e;
    }
    return seen ? -1 : 0;
}

static int same_name(const char* entry, span name) {
    return strncmp(entry, (const char*)name.p, name.n) == 0 && entry[name.n] == '=';
}

#ifndef __APPLE__
/* --- A child measured on its own (CLX-295) --------------------------------------------------
 * On Linux a child's peak memory, wait4's ru_maxrss, is never less than the peak of the memory it
 * ran in before it exec'd: posix_spawn's child runs in this program's memory until then, a fork's
 * in a copy of it, and the kernel keeps that memory's peak as the child's. So a child started
 * with `measured` is started by a helper instead: this program's own file run again, which starts
 * the child before it has done anything else, when it holds a megabyte or two. It starts it with
 * CLONE_PARENT, so it's this program's child as any other is, and its ID, waiting for it, killing
 * it and what wait4 says of it all work as they would. The helper writes the child's ID, why it
 * couldn't run it, 0 when it could, and the time just before it started it, on cx_clock_ns's
 * clock, which is the system's, to a pipe this program gave it, and exits. So the child's wall
 * time doesn't count the helper's starting. */
#define HELPER_NAME "\x01" "calx-measure"

extern int cx_is_program;

typedef struct {
    const char* exe;
    char** argv;
    volatile int err;
} helped;

static int helper_child(void* arg) {
    helped* h = (helped*)arg;
    execve(h->exe, h->argv, environ);
    h->err = errno;
    _exit(127);
}

/* Called first thing as a program starts: when it's the helper, it never returns. Its arguments
 * are the name above, the pipe's number, the program's path and the child's arguments, its
 * name first. The child shares its memory until it execs, as posix_spawn's does, on a stack of
 * its own, and the helper waits until it has. */
void cx_proc_helper(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[0], HELPER_NAME) != 0) return;
    int report = atoi(argv[1]);
    fcntl(report, F_SETFD, FD_CLOEXEC);
    static char stack[65536] __attribute__((aligned(16)));
    helped h = {argv[2], argv + 3, 0};
    cx_u64 at = cx_clock_ns();
    int pid = clone(helper_child, stack + sizeof stack, CLONE_PARENT | CLONE_VM | CLONE_VFORK, &h);
    cx_u64 said[3] = {(cx_u64)(cx_i64)pid, (cx_u64)(pid < 0 ? errno : h.err), at};
    ssize_t w;
    do {
        w = write(report, said, sizeof said);
    } while (w < 0 && errno == EINTR);
    _exit(0);
}

/* Reads what the helper `hpid` said and waits for it to exit: 0 with the child's ID in `pid` and
 * when it started it in `at`, or why it couldn't start the child, whose own exit is reaped here. */
static int helper_said(int fd, pid_t hpid, pid_t* pid, cx_u64* at) {
    cx_u64 said[3] = {0, 0, 0};
    size_t got = 0;
    while (got < sizeof said) {
        ssize_t r = read(fd, (char*)said + got, sizeof said - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    while (waitpid(hpid, NULL, 0) < 0 && errno == EINTR) {
    }
    if (got != sizeof said) return EIO;   /* it ended before it could say */
    pid_t child = (pid_t)(cx_i64)said[0];
    int err = (int)said[1];
    if (child <= 0) return err ? err : EIO;
    if (err) {
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
        return err;
    }
    *pid = child;
    *at = said[2];
    return 0;
}
#endif

static int spawn_os(proc* p, const spec* sp, const cx_u8* input, size_t input_n) {
    int ok = 0;
    char* dir = NULL;
    char* name = cstr(sp->program);
    char** argv = (char**)grow(NULL, (sp->argc + 2) * sizeof *argv);
    char** envp = environ;
    char** made = NULL;   /* the entries the changes add */
    size_t made_n = 0;
    const char* path = getenv("PATH");
    cbuf exe = {0};
    int ours[3] = {-1, -1, -1}, theirs[3] = {-1, -1, -1};
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    argv[0] = name;
    for (size_t i = 0; i < sp->argc; i++) argv[i + 1] = cstr(sp->args[i]);
    argv[sp->argc + 1] = NULL;
    if (sp->has_dir) {
        struct stat st;
        dir = cstr(sp->dir);
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fail(E_BAD_DIR, errno);
            goto done;
        }
    }
    if (sp->clear_env || sp->envc) {
        size_t n = 0;
        if (!sp->clear_env) {
            while (environ[n]) n++;
        }
        envp = (char**)grow(NULL, (n + sp->envc + 1) * sizeof *envp);
        made = (char**)grow(NULL, (sp->envc + 1) * sizeof *made);
        for (size_t i = 0; i < n; i++) envp[i] = environ[i];
        for (size_t c = 0; c < sp->envc; c++) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++) {
                if (!same_name(envp[i], sp->env[c].name)) envp[k++] = envp[i];
            }
            n = k;
            int is_path = sp->env[c].name.n == 4 && memcmp(sp->env[c].name.p, "PATH", 4) == 0;
            if (sp->env[c].set) {
                size_t len = sp->env[c].name.n + 1 + sp->env[c].value.n;
                char* e = (char*)grow(NULL, len + 1);
                memcpy(e, sp->env[c].name.p, sp->env[c].name.n);
                e[sp->env[c].name.n] = '=';
                memcpy(e + sp->env[c].name.n + 1, sp->env[c].value.p, sp->env[c].value.n);
                e[len] = 0;
                made[made_n++] = e;
                envp[n++] = e;
                if (is_path) path = e + 5;
            } else if (is_path) {
                path = getenv("PATH");
            }
        }
        envp[n] = NULL;
    }
    int found = resolve_posix(name, path, &exe);
    if (found <= 0) {
        fail(found < 0 ? E_DENIED : E_NOT_FOUND, found < 0 ? EACCES : ENOENT);
        goto done;
    }
    for (int i = 0; i < 3; i++) {
        switch (sp->io[i]) {
        case IO_PIPE: {
            int fds[2];
#ifdef __APPLE__
            /* macOS has no pipe2. Another thread's child can't take these ends before they're
             * marked, as every child here starts with POSIX_SPAWN_CLOEXEC_DEFAULT. */
            if (pipe(fds) != 0) {
                fail(E_OS, errno);
                goto done;
            }
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#else
            if (pipe2(fds, O_CLOEXEC) != 0) {
                fail(E_OS, errno);
                goto done;
            }
#endif
            ours[i] = i == 0 ? fds[1] : fds[0];
            theirs[i] = i == 0 ? fds[0] : fds[1];
            /* dup2 onto the same number would leave close-on-exec set, and close it. */
            if (theirs[i] <= 2) {
                int d = fcntl(theirs[i], F_DUPFD_CLOEXEC, 3);
                close(theirs[i]);
                theirs[i] = d;
                if (d < 0) {
                    fail(E_OS, errno);
                    goto done;
                }
            }
            fcntl(ours[i], F_SETFL, fcntl(ours[i], F_GETFL) | O_NONBLOCK);
#ifdef __APPLE__
            if (i == 0) fcntl(ours[i], F_SETNOSIGPIPE, 1);  /* see write_quiet */
#endif
            posix_spawn_file_actions_adddup2(&fa, theirs[i], i);
            break;
        }
        case IO_NULL:
            posix_spawn_file_actions_addopen(&fa, i, "/dev/null", i == 0 ? O_RDONLY : O_WRONLY, 0);
            break;
        case IO_STDOUT:
            posix_spawn_file_actions_adddup2(&fa, 1, 2);
            break;
        default:
#ifdef __APPLE__
            /* Kept through CLOEXEC_DEFAULT, where this program has it open. */
            if (fcntl(i, F_GETFD) != -1) posix_spawn_file_actions_addinherit_np(&fa, i);
#endif
            break;
        }
    }
    if (dir) posix_spawn_file_actions_addchdir_np(&fa, dir);
    /* What this program ignores, the child would too: SIGPIPE, which std.http ignores, goes back
     * to ending a program that writes to a closed pipe, as `yes | head` needs. */
    sigset_t def, mask;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGXFSZ);
    sigemptyset(&mask);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setsigmask(&at, &mask);
#ifdef __APPLE__
    /* Only the three streams reach the child, whatever else is open and not yet close-on-exec. */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
#endif
#ifdef __APPLE__
    /* macOS has no affinity a program can set: its scheduler places threads itself. */
    if (sp->ncpus) {
        fail(E_BAD_ARG, 6);
        goto done;
    }
#else
    /* A child to pin is started from this thread pinned to its processors, and takes the thread's
     * affinity with it, so it's pinned before its first instruction. posix_spawn has no attribute
     * for it, and the thread's own goes back after. */
    cpu_set_t was, want;
    int pinned = 0;
    if (sp->ncpus) {
        CPU_ZERO(&want);
        for (size_t i = 0; i < sp->ncpus; i++) {
            if (sp->cpus[i] >= CPU_SETSIZE) {
                fail(E_BAD_ARG, 5);
                goto done;
            }
            CPU_SET(sp->cpus[i], &want);
        }
        if (sched_getaffinity(0, sizeof was, &was) != 0) {
            fail(E_OS, errno);
            goto done;
        }
        if (sched_setaffinity(0, sizeof want, &want) != 0) {
            /* EINVAL: none of them is a processor this program may use. */
            fail(errno == EINVAL ? E_BAD_ARG : E_OS, errno == EINVAL ? 5 : errno);
            goto done;
        }
        pinned = 1;
    }
#endif
#ifndef __APPLE__
    /* A measured child is started by the helper, which this program gives the write end of a pipe
     * as `slot`, a number of its own for now, so the dup2 replaces nothing the child would have
     * had. A Calx library in another program, or one without /proc, starts it as any other. */
    int report[2] = {-1, -1}, slot = -1;
    char** hargv = NULL;
    char slot_text[16];
    if (sp->measured && cx_is_program && access("/proc/self/exe", X_OK) == 0 && pipe2(report, O_CLOEXEC) == 0) {
        slot = fcntl(report[1], F_DUPFD_CLOEXEC, 3);
        if (slot >= 0) {
            snprintf(slot_text, sizeof slot_text, "%d", slot);
            hargv = (char**)grow(NULL, (sp->argc + 5) * sizeof *hargv);
            hargv[0] = (char*)HELPER_NAME;
            hargv[1] = slot_text;
            hargv[2] = exe.p;
            memcpy(hargv + 3, argv, (sp->argc + 2) * sizeof *argv);
            posix_spawn_file_actions_adddup2(&fa, report[1], slot);
        } else {
            close(report[0]);
            close(report[1]);
        }
    }
#endif
    if (sp->io[1] == IO_INHERIT || sp->io[2] == IO_INHERIT || (sp->io[2] == IO_STDOUT && sp->io[1] == IO_INHERIT)) cx_out_flush();
    pid_t pid;
    p->started_ns = cx_clock_ns();
#ifdef __APPLE__
    int e = posix_spawn(&pid, exe.p, &fa, &at, argv, envp);
#else
    int e;
    if (hargv) {
        pid_t hpid;
        e = posix_spawn(&hpid, "/proc/self/exe", &fa, &at, hargv, envp);
        close(report[1]);
        close(slot);
        if (!e) e = helper_said(report[0], hpid, &pid, &p->started_ns);
        close(report[0]);
        free(hargv);
    } else {
        e = posix_spawn(&pid, exe.p, &fa, &at, argv, envp);
    }
    if (pinned) sched_setaffinity(0, sizeof was, &was);
#endif
    if (e) {
        fail(e == ENOENT ? E_NOT_FOUND : e == EACCES || e == ENOEXEC || e == EPERM ? E_DENIED : E_OS, e);
        goto done;
    }
    p->pid = (cx_u32)pid;
#ifdef __APPLE__
    /* A kqueue that's ready once the child exits; one that has exited already is looked for. */
    p->pidfd = kqueue();
    if (p->pidfd >= 0) {
        struct kevent ev;
        EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        fcntl(p->pidfd, F_SETFD, FD_CLOEXEC);
        if (kevent(p->pidfd, &ev, 1, NULL, 0, NULL) != 0) {
            close(p->pidfd);
            p->pidfd = -1;
        }
    }
#else
    p->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
#endif
    p->backoff = 1;
    list_live(p);
    for (int i = 0; i < 3; i++) {
        if (ours[i] < 0) continue;
        p->s[i].fd = ours[i];
        p->s[i].open = 1;
        ours[i] = -1;
    }
    bytes_add(&p->s[0].b, input, input_n);
    if (p->s[0].open) write_some(&p->s[0]);
    ok = 1;
done:
    for (int i = 0; i < 3; i++) {
        if (ours[i] >= 0) close(ours[i]);
        if (theirs[i] >= 0) close(theirs[i]);
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    for (size_t i = 1; i <= sp->argc; i++) free(argv[i]);
    free(argv);
    free(name);
    free(dir);
    for (size_t i = 0; i < made_n; i++) free(made[i]);
    free(made);
    if (envp != environ) free(envp);
    free(exe.p);
    return ok;
}

static void free_os(proc* p) {
    pthread_mutex_lock(&live_lock);
    unlist_locked(p);
    pthread_mutex_unlock(&live_lock);
    for (int i = 0; i < 3; i++) close_stream(&p->s[i]);
    if (p->pidfd >= 0) close(p->pidfd);
    if (!p->exited) {
        int status;
        waitpid((pid_t)p->pid, &status, WNOHANG);
    }
}

static cx_u32 proc_id_os(proc* p) { return p->pid; }

static int which_os(const cx_u8* name, size_t n) {
    span s = {name, n};
    char* c = cstr(s);
    cbuf out = {0};
    int found = resolve_posix(c, getenv("PATH"), &out) > 0;
    if (found) set_result(out.p, out.len);
    free(c);
    free(out.p);
    return found;
}

#endif

/* --- What std.process calls ---------------------------------------------------------------- */

/* A group for children that are waited for together. */
cx_i64 cx_proc_group(void) { return (cx_i64)(intptr_t)group_new(); }

void cx_proc_group_free(cx_i64 g) {
    if (g) group_release((group*)(intptr_t)g);
}

cx_u32 cx_proc_error_kind(void) { return err_kind; }
cx_i32 cx_proc_error_code(void) { return err_code; }

/* Starts the command `spec_p`, which std.process wrote and checked, writing `input` to it if its
 * input is a pipe. Returns the child, or 0 with why in `cx_proc_error_kind`. With `g` 0 the child
 * waits alone. */
cx_i64 cx_proc_spawn(const cx_u8* spec_p, cx_usize spec_n, const cx_u8* input, cx_usize input_n, cx_i64 g) {
    spec sp;
    if (!spec_read(&sp, spec_p, spec_n)) {
        spec_free(&sp);
        fail(E_BAD_ARG, 1);
        return 0;
    }
    proc* p = (proc*)calloc(1, sizeof *p);
    if (!p) out_of_memory();
    p->g = g ? (group*)(intptr_t)g : group_new();
    if (!p->g) {
        fail(E_OS, 0);
        free(p);
        spec_free(&sp);
        return 0;
    }
    if (g) p->g->refs++;
    for (int i = 0; i < 3; i++) {
        p->s[i].index = i;
        p->s[i].owner = p;
#ifndef _WIN32
        p->s[i].fd = -1;
#endif
    }
#ifndef _WIN32
    p->pidfd = -1;
#endif
    if (sp.timeout_ms) {
        cx_u64 now = cx_clock_ns();
        p->deadline = sp.timeout_ms > (UINT64_MAX - now) / 1000000 ? UINT64_MAX : now + sp.timeout_ms * 1000000;
    }
    int ok = spawn_os(p, &sp, input, input_n);
    spec_free(&sp);
    if (!ok) {
        for (int i = 0; i < 3; i++) free(p->s[i].b.p);
#ifdef _WIN32
        free(p->s[0].more.p);
#endif
        group_release(p->g);
        free(p);
        return 0;
    }
    return (cx_i64)(intptr_t)p;
}

static int finished(const proc* p) {
#ifdef _WIN32
    for (int i = 0; i < 3; i++) {
        if (p->s[i].open || p->s[i].pending) return 0;
    }
#else
    for (int i = 0; i < 3; i++) {
        if (p->s[i].open) return 0;
    }
#endif
    return p->exited;
}

static int ended(const stream* s) {
#ifdef _WIN32
    return !s->open && !s->pending;
#else
    return !s->open;
#endif
}

void cx_proc_close_input(cx_i64 h) {
    proc* p = P(h);
    p->close_input = 1;
    if (!queued(&p->s[0])) close_stream(&p->s[0]);
}

/* Writes `n` bytes to the child's input, reading what it writes meanwhile. 0, or E_CLOSED if it
 * stopped reading first. */
cx_i32 cx_proc_write(cx_i64 h, const cx_u8* d, cx_usize n) {
    proc* p = P(h);
    stream* s = &p->s[0];
    if (!s->open) return E_CLOSED;
#ifdef _WIN32
    bytes_add(&s->more, d, n);
    start_write(s);
    /* A write cancelled as the child exits says how it went when its completion comes. */
    while (queued(s) || s->pending) pump(&p, 1, 0);
#else
    bytes_add(&s->b, d, n);
    write_some(s);
    while (queued(s)) pump(&p, 1, 0);
#endif
    return s->broken ? E_CLOSED : 0;
}

/* Waits for the child to finish. */
void cx_proc_wait(cx_i64 h) {
    proc* p = P(h);
    while (!finished(p)) pump(&p, 1, 0);
}

/* Waits for one of the `n` children, which share a group, to finish, and says which. */
cx_usize cx_proc_wait_any(const cx_i64* hs, cx_usize n) {
    proc* small[16];
    proc** ps = n <= 16 ? small : (proc**)grow(NULL, n * sizeof *ps);
    for (cx_usize i = 0; i < n; i++) ps[i] = P(hs[i]);
    cx_usize done = n;
    while (done == n) {
        for (cx_usize i = 0; i < n && done == n; i++) {
            if (finished(ps[i])) done = i;
        }
        if (done == n) pump(ps, n, 0);
    }
    if (ps != small) free(ps);
    return done;
}

/* Handles what has happened without waiting, and says whether the child has exited. */
cx_i32 cx_proc_poll(cx_i64 h) {
    proc* p = P(h);
    pump(&p, 1, 1);
    return p->exited;
}

/* Waits until a line has come from the child's output, until `until` on cx_clock_ns's clock (0
 * for no limit). The line's length with its `\n`, or at the end the length of what's left
 * without one; -1 if `until` came first, and -2 at the end with nothing left. */
cx_i64 cx_proc_line(cx_i64 h, cx_u64 until) {
    proc* p = P(h);
    stream* s = &p->s[1];
    for (;;) {
        bytes* b = &s->b;
        if (s->scan < b->start) s->scan = b->start;
        const cx_u8* nl = s->scan < b->len ? (const cx_u8*)memchr(b->p + s->scan, '\n', b->len - s->scan) : NULL;
        if (nl) return (cx_i64)(nl - (b->p + b->start)) + 1;
        s->scan = b->len;
        if (ended(s)) return b->len > b->start ? (cx_i64)(b->len - b->start) : -2;
        if (until && cx_clock_ns() >= until) return -1;
        pump(&p, 1, until);
    }
}

/* Waits until some output has come: how much, -1 if `until` came first, -2 at the end. */
cx_i64 cx_proc_some(cx_i64 h, cx_u64 until) {
    proc* p = P(h);
    stream* s = &p->s[1];
    for (;;) {
        if (s->b.len > s->b.start) return (cx_i64)(s->b.len - s->b.start);
        if (ended(s)) return -2;
        if (until && cx_clock_ns() >= until) return -1;
        pump(&p, 1, until);
    }
}

/* The bytes of stream 1 or 2 that came and weren't taken. */
cx_usize cx_proc_available(cx_i64 h, cx_u32 i) {
    stream* s = &P(h)->s[i];
    return s->b.len - s->b.start;
}

/* Takes the first `n` of them. */
void cx_proc_take(cx_i64 h, cx_u32 i, cx_u8* to, cx_usize n) {
    stream* s = &P(h)->s[i];
    if (n) memcpy(to, s->b.p + s->b.start, n);
    s->b.start += n;
}

void cx_proc_kill(cx_i64 h) { kill_proc(P(h)); }

cx_u32 cx_proc_id(cx_i64 h) { return proc_id_os(P(h)); }

cx_u32 cx_proc_state(cx_i64 h) {
    proc* p = P(h);
    return (p->exited ? ST_EXITED : 0) | (p->killed ? ST_KILLED : 0) | (p->timed_out ? ST_TIMED_OUT : 0) | (finished(p) ? ST_FINISHED : 0) |
           (p->exited && p->has_code ? ST_HAS_CODE : 0);
}

cx_i32 cx_proc_code(cx_i64 h) { return P(h)->code; }
cx_i32 cx_proc_signal(cx_i64 h) { return P(h)->signal; }

/* What the child has used, into `out`: its CPU time in ns, its memory in RAM now and at its peak,
 * in bytes, and its wall time in ns. Once it has exited, what it used in all, as recorded when it
 * ended, with nothing in RAM now. It's looked at first, so a child that has just exited gives its
 * totals. */
void cx_proc_usage(cx_i64 h, cx_u64* out) {
    proc* p = P(h);
    if (!p->exited) pump(&p, 1, 1);
    if (p->exited) {
        out[0] = p->cpu_ns;
        out[1] = 0;
        out[2] = p->peak_memory;
        out[3] = p->ended_ns - p->started_ns;
        return;
    }
    usage_os(p, out);
    out[3] = cx_clock_ns() - p->started_ns;
}

/* Lets the child go: its handles and buffers. One that hasn't exited runs on. */
void cx_proc_free(cx_i64 h) {
    proc* p = P(h);
    if (!p) return;
    free_os(p);
    for (int i = 0; i < 3; i++) free(p->s[i].b.p);
    group_release(p->g);
    free(p);
}

/* Finds a program as `cx_proc_spawn` would, with this program's PATH: 1 with its path for
 * `cx_proc_result_copy`, or 0. */
cx_i32 cx_proc_which(const cx_u8* name, cx_usize n) { return which_os(name, n); }

cx_usize cx_proc_result_len(void) { return result_len; }
void cx_proc_result_copy(cx_u8* to) {
    if (result_len) memcpy(to, result, result_len);
}

/* This program's own executable, as a full path, for `cx_proc_result_copy`. */
cx_i32 cx_proc_exe(void) {
#ifdef _WIN32
    wchar_t small[MAX_PATH];
    wchar_t* w = small;
    DWORD cap = MAX_PATH, n;
    for (;;) {
        n = GetModuleFileNameW(NULL, w, cap);
        if (n == 0) {
            if (w != small) free(w);
            return 0;
        }
        if (n < cap) break;
        cap *= 2;
        w = (wchar_t*)grow(w == small ? NULL : w, cap * sizeof(wchar_t));
    }
    int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
    char* u = (char*)grow(NULL, (size_t)k);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, u, k, NULL, NULL);
    set_result(u, (size_t)k);
    free(u);
    if (w != small) free(w);
    return 1;
#elif defined(__APPLE__)
    /* The path it was started by, which may go through links, so resolved, as /proc/self/exe is. */
    uint32_t cap = 0;
    _NSGetExecutablePath(NULL, &cap);
    char* b = (char*)grow(NULL, (size_t)cap + 1);
    if (_NSGetExecutablePath(b, &cap) != 0) {
        free(b);
        return 0;
    }
    char* full = realpath(b, NULL);
    free(b);
    if (!full) return 0;
    set_result(full, strlen(full));
    free(full);
    return 1;
#else
    size_t cap = 256;
    char* b = NULL;
    for (;;) {
        b = (char*)grow(b, cap);
        ssize_t n = readlink("/proc/self/exe", b, cap);
        if (n < 0) {
            free(b);
            return 0;
        }
        if ((size_t)n < cap) {
            set_result(b, (size_t)n);
            free(b);
            return 1;
        }
        cap *= 2;
    }
#endif
}

/* Pins this program to the `n` processors at `cpus`, which std.process checked are some: on
 * Windows the whole process, and on Linux the thread that calls, which the programs and threads it
 * starts after take with them, as Python's `os.sched_setaffinity(0, ...)` does. 0 when it's done,
 * or why not, as `cx_proc_spawn` says. */
cx_i32 cx_proc_set_affinity(const cx_u32* cpus, cx_usize n) {
#ifdef _WIN32
    DWORD_PTR mine, machine, pin = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &mine, &machine)) machine = 0;
    for (cx_usize i = 0; i < n; i++) {
        if (cpus[i] >= 8 * sizeof(DWORD_PTR) || !((machine >> cpus[i]) & 1)) {
            fail(E_BAD_ARG, 5);
            return -1;
        }
        pin |= (DWORD_PTR)1 << cpus[i];
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), pin)) {
        fail(E_OS, (cx_i32)GetLastError());
        return -1;
    }
    return 0;
#elif defined(__APPLE__)
    (void)cpus;
    (void)n;
    fail(E_BAD_ARG, 6);
    return -1;
#else
    cpu_set_t want;
    CPU_ZERO(&want);
    for (cx_usize i = 0; i < n; i++) {
        if (cpus[i] >= CPU_SETSIZE) {
            fail(E_BAD_ARG, 5);
            return -1;
        }
        CPU_SET(cpus[i], &want);
    }
    if (sched_setaffinity(0, sizeof want, &want) != 0) {
        fail(errno == EINVAL ? E_BAD_ARG : E_OS, errno == EINVAL ? 5 : errno);
        return -1;
    }
    return 0;
#endif
}

/* Takes this program out of Windows' power throttling, as a child's `full_speed` does. Elsewhere
 * there's none to take it out of. */
void cx_proc_full_speed(void) {
#ifdef _WIN32
    full_speed_win(GetCurrentProcess());
#endif
}

/* The machine's CPU time since it started, summed over its processors, in ns: how much went to
 * running anything, `out[0]`, and how much was idle, `out[1]`. Both 0 where the system can't say.
 * Windows counts in 100 ns; Linux and macOS in clock ticks, 100 a second on most. */
void cx_proc_machine_cpu(cx_u64* out) {
    out[0] = out[1] = 0;
#ifdef _WIN32
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return;
    cx_u64 i = ((cx_u64)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
    cx_u64 k = ((cx_u64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    cx_u64 u = ((cx_u64)user.dwHighDateTime << 32) | user.dwLowDateTime;
    /* The kernel's time has the idle time in it. */
    out[0] = (k + u - i) * 100;
    out[1] = i * 100;
#elif defined(__APPLE__)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS) return;
    long hz = sysconf(_SC_CLK_TCK);
    cx_u64 tick = hz > 0 ? 1000000000ull / (cx_u64)hz : 10000000ull;
    out[0] = ((cx_u64)info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE]) * tick;
    out[1] = (cx_u64)info.cpu_ticks[CPU_STATE_IDLE] * tick;
#else
    /* /proc/stat's first line: user, nice, system, idle, iowait, irq, softirq, steal, then guest
     * and guest_nice, which user and nice count already. */
    char b[512];
    int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t got = read(fd, b, sizeof b - 1);
    close(fd);
    if (got < 5 || memcmp(b, "cpu ", 4) != 0) return;
    b[got] = 0;
    cx_u64 f[8] = {0};
    char* s = b + 4;
    for (int i = 0; i < 8; i++) {
        while (*s == ' ') s++;
        if (*s < '0' || *s > '9') break;
        while (*s >= '0' && *s <= '9') f[i] = f[i] * 10 + (cx_u64)(*s++ - '0');
    }
    long hz = sysconf(_SC_CLK_TCK);
    cx_u64 tick = hz > 0 ? 1000000000ull / (cx_u64)hz : 10000000ull;
    out[0] = (f[0] + f[1] + f[2] + f[5] + f[6] + f[7]) * tick;
    out[1] = (f[3] + f[4]) * tick;
#endif
}
