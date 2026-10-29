/* The Calx runtime's compiled part. The driver builds this once per target and build mode and
 * links it into every program. */
#ifndef _WIN32
#define _GNU_SOURCE /* sched_getaffinity */
#endif
#include "calx_rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* WaitOnAddress and WakeByAddressAll, for the workers of `parallel for`. */
#pragma comment(lib, "synchronization.lib")
#else
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* --- output ------------------------------------------------------------------------------- */

/* Standard output's buffer belongs to the main thread, and to the threads `Thread.spawn` starts
 * (D121), which take a lock once there are any. Inside a `parallel for`, every thread writes to
 * the buffer of the chunk it's running instead (see "parallel for" below). */
static unsigned char out_buf[1 << 16];
static size_t out_len;
static _Thread_local int to_stderr; /* while `eprint` or a panic message is being written */
static _Thread_local cx_String* fmt_to; /* while a string is being interpolated */

static _Thread_local int par_buffered; /* output goes to the running chunk's buffer */
static _Thread_local int par_depth;    /* inside a `parallel for`, even one run on this thread alone */
static void par_write(int stream, const void* p, size_t n);

static int out_shared; /* a thread has been spawned, so writing takes the lock */
#ifdef _WIN32
static SRWLOCK out_mutex = SRWLOCK_INIT;
#else
static pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Read once, so a lock and its unlock agree. */
static int out_lock(void) {
    int shared = __atomic_load_n(&out_shared, __ATOMIC_ACQUIRE);
#ifdef _WIN32
    if (shared) AcquireSRWLockExclusive(&out_mutex);
#else
    if (shared) pthread_mutex_lock(&out_mutex);
#endif
    return shared;
}

static void out_unlock(int shared) {
#ifdef _WIN32
    if (shared) ReleaseSRWLockExclusive(&out_mutex);
#else
    if (shared) pthread_mutex_unlock(&out_mutex);
#endif
}

static void flush_locked(void) {
    if (out_len) {
        fwrite(out_buf, 1, out_len, stdout);
        out_len = 0;
    }
    fflush(stdout);
}

static void flush_out(void) {
    int l = out_lock();
    flush_locked();
    out_unlock(l);
}

static void out_bytes(const void* p, size_t n) {
    if (n == 0) return; /* an empty string's pointer can be null, which memcpy can't take */
    int l = out_lock();
    if (n > sizeof out_buf - out_len) {
        flush_locked();
        if (n > sizeof out_buf) {
            fwrite(p, 1, n, stdout);
            out_unlock(l);
            return;
        }
    }
    memcpy(out_buf + out_len, p, n);
    out_len += n;
    out_unlock(l);
}

/* Standard error, after whatever standard output has waiting. */
static void err_bytes(const void* p, size_t n) {
    int l = out_lock();
    fwrite(p, 1, n, stderr);
    out_unlock(l);
}

static void write_bytes(const void* p, size_t n) {
    if (n == 0) return;
    if (fmt_to) {
        cx_string_push_bytes(fmt_to, (const cx_u8*)p, n);
    } else if (par_buffered) {
        par_write(to_stderr, p, n);
    } else if (to_stderr) {
        err_bytes(p, n);
    } else {
        out_bytes(p, n);
    }
}

/* --- the end of the program ---------------------------------------------------------------
 * With threads, two could end the program at once: one returning from `main` and one
 * panicking, or two panicking. The first to get here does, and any other waits for it to. */

static int exit_taken;
static _Thread_local int exit_mine;

static void park(void) {
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

static void take_exit(void) {
    if (exit_mine) return;
    if (__atomic_exchange_n(&exit_taken, 1, __ATOMIC_ACQ_REL)) park();
    exit_mine = 1;
}

/* The threads `Thread.spawn` started that are still running, which own memory the leak check
 * would count, and whether this is one of them. */
static cx_usize services_live;
static _Thread_local int par_service;

void cx_service_started(void) {
    __atomic_store_n(&out_shared, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&services_live, 1, __ATOMIC_ACQ_REL);
}

void cx_service_enter(void) { par_service = 1; }

void cx_service_leave(void) { __atomic_fetch_sub(&services_live, 1, __ATOMIC_ACQ_REL); }

/* The stack each thread of a Calx program has (D135): 8 MiB, the main thread's on Linux and macOS,
 * and on Windows too, where the driver links programs so. Windows would make it 1 MiB, glibc what
 * `ulimit -s` says and macOS 512 KiB, so the same program could recurse deeply enough on one
 * system and overflow on another. It's address space kept for the stack, not memory: pages are
 * only taken as the stack grows into them. */
#define CX_THREAD_STACK ((cx_usize)8 << 20)

/* A call that goes past the end of the stack panics, as any other bug does, with the message and
 * the trace. The panic runs on stack kept for it: on Windows the end of the thread's own, which
 * `SetThreadStackGuarantee` holds back until the stack overflows, and elsewhere a stack of its
 * own the signal is taken on. Both are this big, room for the trace to read the line tables. */
#define CX_PANIC_STACK ((cx_usize)256 << 10)

CX_COLD void cx_panic_text(const char* at, const char* message);

static CX_COLD void overflow_panic(void) {
    cx_panic_text("<stack>", "the calls went deeper than the thread's 8 MiB stack holds");
}

#ifdef _WIN32
static LONG WINAPI overflow_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) overflow_panic();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* On each thread as it starts. */
static void overflow_thread_begin(void) {
    ULONG keep = (ULONG)CX_PANIC_STACK;
    SetThreadStackGuarantee(&keep);
}

static void overflow_thread_end(void) {}

static void overflow_start(void) {
    AddVectoredExceptionHandler(1, overflow_handler);
    overflow_thread_begin();
}
#else
/* The lowest address of this thread's stack, and the stack the signal is taken on. */
static _Thread_local uintptr_t stack_low;
static _Thread_local void* signal_stack;

/* A fault just below the stack's end is the stack overflowing into the pages kept below it, which
 * are a page for a thread and a megabyte for the main thread. Any other is left to the system,
 * which ends the program as it would without the handler. */
static void overflow_handler(int sig, siginfo_t* info, void* context) {
    (void)context;
    uintptr_t at = (uintptr_t)info->si_addr, low = stack_low;
    if (info->si_code > 0 && low && at < low + 4096 && low - at <= ((uintptr_t)2 << 20)) overflow_panic();
    signal(sig, SIG_DFL);
    if (info->si_code <= 0) raise(sig);
}

static void overflow_thread_begin(void) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* low;
        size_t size;
        if (pthread_attr_getstack(&attr, &low, &size) == 0) stack_low = (uintptr_t)low;
        pthread_attr_destroy(&attr);
    }
    void* p = mmap(NULL, CX_PANIC_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t s = {.ss_sp = p, .ss_size = CX_PANIC_STACK, .ss_flags = 0};
    if (sigaltstack(&s, NULL) == 0) signal_stack = p;
    else munmap(p, CX_PANIC_STACK);
}

static void overflow_thread_end(void) {
    if (!signal_stack) return;
    stack_t off = {.ss_sp = NULL, .ss_size = 0, .ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    munmap(signal_stack, CX_PANIC_STACK);
    signal_stack = NULL;
}

static void overflow_start(void) {
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_sigaction = overflow_handler;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&a.sa_mask);
    sigaction(SIGSEGV, &a, NULL);
    sigaction(SIGBUS, &a, NULL);
    overflow_thread_begin();
}
#endif

/* Starts a thread on `run(arg)` with that stack, giving it in `*thread`: 0, or the error,
 * `GetLastError`'s or `pthread_create`'s. */
#ifdef _WIN32
typedef struct {
    LPTHREAD_START_ROUTINE run;
    void* arg;
} os_start;

static DWORD WINAPI os_thread_main(LPVOID p) {
    os_start s = *(os_start*)p;
    free(p);
    overflow_thread_begin();
    DWORD r = s.run(s.arg);
    overflow_thread_end();
    return r;
}

int cx_os_thread(HANDLE* thread, LPTHREAD_START_ROUTINE run, void* arg) {
    os_start* s = (os_start*)malloc(sizeof *s);
    if (!s) return ERROR_NOT_ENOUGH_MEMORY;
    s->run = run;
    s->arg = arg;
    *thread = CreateThread(NULL, CX_THREAD_STACK, os_thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (*thread) return 0;
    free(s);
    return (int)GetLastError();
}
#else
typedef struct {
    void* (*run)(void*);
    void* arg;
} os_start;

static void* os_thread_main(void* p) {
    os_start s = *(os_start*)p;
    free(p);
    overflow_thread_begin();
    void* r = s.run(s.arg);
    overflow_thread_end();
    return r;
}

int cx_os_thread(pthread_t* thread, void* (*run)(void*), void* arg) {
    os_start* s = (os_start*)malloc(sizeof *s);
    if (!s) return ENOMEM;
    s->run = run;
    s->arg = arg;
    pthread_attr_t attr;
    int e = pthread_attr_init(&attr);
    if (!e) {
        e = pthread_attr_setstacksize(&attr, CX_THREAD_STACK);
        if (!e) e = pthread_create(thread, &attr, os_thread_main, s);
        pthread_attr_destroy(&attr);
    }
    if (e) free(s);
    return e;
}
#endif

/* Text for a panic message, or anything else written the way `print` writes. */
static void panic_text(const char* s) { write_bytes(s, strlen(s)); }

/* --- std.env: the command line, as UTF-8 ------------------------------------------------- */

typedef struct {
    cx_u8* ptr;
    cx_usize len;
} arg_text;

static arg_text* args;
static cx_usize arg_count;

static void* must_alloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) {
        fputs("calx: out of memory reading the command line\n", stderr);
        exit(101);
    }
    return p;
}

#ifdef _WIN32
/* Windows passes the command line as one UTF-16 string, so it's split here by the rules the
 * C runtime documents ("Parsing C command-line arguments"): the program's name ends at the next
 * quote if it starts with one, and at whitespace if not. Each later argument ends at whitespace
 * outside quotes. Backslashes are literal unless they come before a quote: then each pair gives
 * one backslash, and an odd one left over makes the quote literal. Inside quotes, `""` is a
 * literal quote. Converting to UTF-8 turns an unpaired surrogate into U+FFFD. */
static void add_wide_arg(const wchar_t* w, int n) {
    int bytes = n ? WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL) : 0;
    arg_text* a = &args[arg_count++];
    a->ptr = must_alloc((size_t)bytes);
    a->len = (cx_usize)(bytes ? WideCharToMultiByte(CP_UTF8, 0, w, n, (char*)a->ptr, bytes, NULL, NULL) : 0);
}

static void read_args(void) {
    const wchar_t* p = GetCommandLineW();
    size_t n = wcslen(p);
    wchar_t* buf = must_alloc((n + 1) * sizeof(wchar_t));
    args = must_alloc((n / 2 + 2) * sizeof(arg_text)); /* each argument after the first takes 2 characters */
    int k = 0;
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) buf[k++] = *p;
        if (*p) p++;
    } else {
        for (; *p && *p != L' ' && *p != L'\t'; p++) buf[k++] = *p;
    }
    add_wide_arg(buf, k);
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        int quoted = 0;
        k = 0;
        for (;;) {
            size_t slashes = 0;
            while (*p == L'\\') slashes++, p++;
            if (*p == L'"') {
                for (size_t j = 0; j < slashes / 2; j++) buf[k++] = L'\\';
                if (slashes % 2) {
                    buf[k++] = L'"';
                    p++;
                } else if (quoted && p[1] == L'"') {
                    buf[k++] = L'"';
                    p += 2;
                } else {
                    quoted = !quoted;
                    p++;
                }
                continue;
            }
            for (size_t j = 0; j < slashes; j++) buf[k++] = L'\\';
            if (!*p || (!quoted && (*p == L' ' || *p == L'\t'))) break;
            buf[k++] = *p++;
        }
        add_wide_arg(buf, k);
    }
    free(buf);
}
#else
/* Linux passes bytes. Any that aren't UTF-8 become U+FFFD, one for each byte that doesn't start
 * a valid sequence, so every argument is a valid `String`. */
static void read_args(int argc, char** argv) {
    args = must_alloc((size_t)(argc > 0 ? argc : 1) * sizeof(arg_text));
    for (int i = 0; i < argc; i++) {
        const cx_u8* s = (const cx_u8*)argv[i];
        size_t n = strlen(argv[i]);
        cx_u8* out = must_alloc(n * 3);
        size_t len = 0;
        for (size_t at = 0; at < n;) {
            cx_u8 b = s[at];
            size_t w = b < 0x80 ? 1 : b >= 0xC2 && b <= 0xDF ? 2 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 0;
            if (w && at + w <= n && cx_utf8_valid(s + at, w)) {
                memcpy(out + len, s + at, w);
                len += w;
                at += w;
            } else {
                memcpy(out + len, "\xEF\xBF\xBD", 3);
                len += 3;
                at++;
            }
        }
        args[arg_count].ptr = out;
        args[arg_count++].len = len;
    }
}
#endif

/* Declared by std/env.calx. Argument 0 is the program's name. */
cx_usize cx_arg_count(void) { return arg_count; }
const cx_u8* cx_arg(cx_usize i) { return args[i].ptr; }
cx_usize cx_arg_len(cx_usize i) { return args[i].len; }

/* Declared by std/env.calx. The value of the environment variable named by the `n` bytes of
 * UTF-8 at `name`, written to `out` if its UTF-8 fits in `cap` bytes: its length in bytes, which
 * is more than `cap` if it didn't fit, or -1 if it isn't set or the name can't be one. Windows
 * holds the environment as UTF-16, whose unpaired surrogates become U+FFFD; Linux holds bytes,
 * and each that doesn't start a valid UTF-8 sequence becomes U+FFFD, as in the arguments. */
cx_i64 cx_env_get(const cx_u8* name, cx_usize n, cx_u8* out, cx_usize cap) {
    if (n == 0 || n > 32767 || memchr(name, 0, n) || memchr(name, '=', n)) return -1;
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char*)name, (int)n, NULL, 0);
    wchar_t* wname = must_alloc(((size_t)wn + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, (const char*)name, (int)n, wname, wn);
    wname[wn] = 0;
    /* With no room, Windows gives the size the value needs with its terminator, 1 for an empty
     * value, and 0 for one that isn't set. Another thread can change it between the two calls. */
    wchar_t* value = NULL;
    DWORD len = 0;
    DWORD size = GetEnvironmentVariableW(wname, NULL, 0);
    while (size != 0) {
        free(value);
        value = must_alloc((size_t)size * sizeof(wchar_t));
        SetLastError(0);
        len = GetEnvironmentVariableW(wname, value, size);
        if (len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) size = 0;
        else if (len < size) break;
        else size = len;
    }
    free(wname);
    if (size == 0) {
        free(value);
        return -1;
    }
    int bytes = len ? WideCharToMultiByte(CP_UTF8, 0, value, (int)len, NULL, 0, NULL, NULL) : 0;
    if (bytes > 0 && (cx_usize)bytes <= cap) WideCharToMultiByte(CP_UTF8, 0, value, (int)len, (char*)out, bytes, NULL, NULL);
    free(value);
    return bytes;
#else
    char* cname = must_alloc(n + 1);
    memcpy(cname, name, n);
    cname[n] = 0;
    const cx_u8* s = (const cx_u8*)getenv(cname);
    free(cname);
    if (!s) return -1;
    size_t len = 0;
    size_t sn = strlen((const char*)s);
    for (size_t at = 0; at < sn;) {
        cx_u8 b = s[at];
        size_t w = b < 0x80 ? 1 : b >= 0xC2 && b <= 0xDF ? 2 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 0;
        if (w && at + w <= sn && cx_utf8_valid(s + at, w)) {
            if (len + w <= cap) memcpy(out + len, s + at, w);
            len += w;
            at += w;
        } else {
            if (len + 3 <= cap) memcpy(out + len, "\xEF\xBF\xBD", 3);
            len += 3;
            at++;
        }
    }
    return (cx_i64)len;
#endif
}

/* --- std.time ------------------------------------------------------------------------------ */

#ifdef _WIN32
static cx_u64 counter_hz;
#endif

/* Declared by std/time.calx: nanoseconds on a clock that never goes backward. */
cx_u64 cx_clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    cx_u64 c = (cx_u64)now.QuadPart;
    return c / counter_hz * 1000000000u + c % counter_hz * 1000000000u / counter_hz;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (cx_u64)ts.tv_sec * 1000000000u + (cx_u64)ts.tv_nsec;
#endif
}

/* Declared by std/time.calx: the wall clock, as Java's `System.currentTimeMillis`. */
cx_i64 cx_clock_unix_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    /* 100-nanosecond ticks since 1601, which is 11644473600 seconds before 1970. */
    return (cx_i64)(t.QuadPart / 10000u) - 11644473600000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (cx_i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Declared by std/time.calx: the seconds the local time zone is ahead of UTC at `unix_ms`, by
 * the system's rules, so summer time counts. 0 where the system can't say. */
cx_i32 cx_clock_local_offset(cx_i64 unix_ms) {
    cx_i64 secs = unix_ms / 1000 - (unix_ms % 1000 < 0);
    time_t t = (time_t)secs;
    struct tm local;
#ifdef _WIN32
    if (localtime_s(&local, &t) != 0) return 0;
    /* The local fields read as if they were UTC, less the moment itself. */
    __time64_t as_utc = _mkgmtime64(&local);
    if (as_utc == -1) return 0;
    return (cx_i32)(as_utc - (__time64_t)t);
#else
    if (!localtime_r(&t, &local)) return 0;
    return (cx_i32)local.tm_gmtoff;
#endif
}

/* Declared by std/time.calx. The local time zone's abbreviation at a moment, as the system's
 * time zone database names it, such as "CET" or "+03", written to `out` if it fits in `cap`
 * bytes: its length in bytes, more than `cap` if it didn't fit, or -1 if there's none. Windows
 * has none: it names zones only in full, such as "W. Europe Standard Time". */
cx_i64 cx_clock_zone_abbreviation(cx_i64 unix_ms, cx_u8* out, cx_usize cap) {
#ifdef _WIN32
    (void)unix_ms;
    (void)out;
    (void)cap;
    return -1;
#else
    cx_i64 secs = unix_ms / 1000 - (unix_ms % 1000 < 0);
    time_t t = (time_t)secs;
    struct tm local;
    if (!localtime_r(&t, &local) || !local.tm_zone || !local.tm_zone[0]) return -1;
    size_t len = strlen(local.tm_zone);
    if (len <= cap) memcpy(out, local.tm_zone, len);
    return (cx_i64)len;
#endif
}

void cx_start(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    /* The same bytes on every platform: no "\n" to "\r\n" translation, and UTF-8 in a console. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    counter_hz = (cx_u64)hz.QuadPart;
    read_args();
#else
    read_args(argc, argv);
#endif
    overflow_start();
}

int cx_finish(void) {
    take_exit();
    flush_out();
#ifdef CX_DEBUG
    /* Debug builds count the blocks still allocated, and a program that ends with any left has
     * leaked: the compiler drops every value that owns memory (D76), so that's a compiler bug.
     * A thread still running owns what it holds, and stops where it is, so then there's no check. */
    cx_usize live = cx_alloc_live();
    if (live && !__atomic_load_n(&services_live, __ATOMIC_ACQUIRE)) {
        fprintf(stderr, "calx: %llu allocations were never freed; this is a bug in calx, please report it\n",
                (unsigned long long)live);
        return 102;
    }
#endif
    return 0;
}

void cx_print_str(cx_str s) { write_bytes(s.ptr, s.len); }

/* --- std.io --------------------------------------------------------------------------------
 * These are declared by std/io.calx as C functions, not in calx_rt.h. */

cx_out_to cx_err_begin(void) {
    cx_out_to was = {fmt_to, to_stderr};
    if (out_len && !par_buffered) flush_out();
    fmt_to = 0;
    to_stderr = 1;
    return was;
}

void cx_err_end(cx_out_to was) {
    fmt_to = (cx_String*)was.fmt;
    to_stderr = was.err;
}

void cx_out_write(const cx_u8* p, cx_usize n) {
    if (par_buffered) par_write(0, p, n);
    else out_bytes(p, n);
}

/* Inside a `parallel for`, output is written when the loop ends, in order. */
void cx_out_flush(void) {
    if (!par_buffered) flush_out();
}

/* Whether standard output is a terminal. A Windows console reads ANSI escape codes only once a
 * program turns that on, which this does, and a console that can't, before Windows 10, counts as
 * no terminal, so a program that colors only a terminal's text never shows the codes raw. */
cx_u32 cx_out_is_terminal(void) {
#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return 0;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return 1;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? 1 : 0;
#else
    return isatty(1) ? 1 : 0;
#endif
}

/* Reads at most `n` bytes, returning 0 only at the end of the input. Output waiting in the
 * buffer is written first, since the program may be waiting for an answer to it. */
cx_usize cx_in_read(cx_u8* p, cx_usize n) {
    if (par_depth) {
        /* Which iteration got which bytes would depend on timing. One thread or many, it's the same panic. */
        cx_panic_begin("<stdin>");
        panic_text("can't read standard input inside a `parallel for`");
        cx_panic_end();
    }
    if (n == 0) return 0;
    if (out_len) flush_out();
#ifdef _WIN32
    int r = _read(0, p, n > 0x40000000 ? 0x40000000u : (unsigned)n);
#else
    ssize_t r;
    do {
        r = read(0, p, n > 0x40000000 ? 0x40000000 : n);
    } while (r < 0 && errno == EINTR);
#endif
    if (r < 0) {
        cx_panic_begin("<stdin>");
        panic_text("can't read standard input");
        cx_panic_end();
    }
    return (cx_usize)r;
}

void cx_print_u64(cx_u64 v) {
    char buf[24];
    int i = sizeof buf;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    write_bytes(buf + i, sizeof buf - (size_t)i);
}

void cx_print_i64(cx_i64 v) {
    if (v < 0) {
        write_bytes("-", 1);
        cx_print_u64((cx_u64)0 - (cx_u64)v);
    } else {
        cx_print_u64((cx_u64)v);
    }
}

void cx_print_bool(cx_bool v) {
    if (v) write_bytes("true", 4);
    else write_bytes("false", 5);
}

void cx_print_rune(cx_rune c) {
    unsigned char b[4];
    size_t n;
    if (c < 0x80) {
        b[0] = (unsigned char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (unsigned char)(0xC0 | (c >> 6));
        b[1] = (unsigned char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (c >> 12));
        b[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (c >> 18));
        b[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (c & 0x3F));
        n = 4;
    }
    write_bytes(b, n);
}

/* A lone UTF-16 surrogate can't be written as UTF-8, so it prints as U+FFFD. */
void cx_print_char(cx_char c) { cx_print_rune(c >= 0xD800 && c <= 0xDFFF ? 0xFFFD : c); }

void cx_print_newline(void) { write_bytes("\n", 1); }

/* --- floats as Java prints them --------------------------------------------------------------
 *
 * Java's Double.toString and Float.toString (JDK 19 and later) print the shortest decimal that
 * reads back as the same value, choosing the closest one when there are several, and a 2-digit
 * one when 1 digit would do. Between 10^-3 and 10^7 the decimal is written plainly with at least
 * one digit after the point, and otherwise as d.dddE<n>.
 *
 * This finds the shortest correctly rounded decimal with snprintf and strtod. It's provisional:
 * the rig's float formatting probe checks it against Java, and it may be replaced by a direct
 * shortest-digits algorithm for speed. */

/* Writes the significant digits of |v| into digits (no point) and returns the decimal exponent
 * of the first digit. */
static int shortest(double v, int is_float, char* digits) {
    char buf[40];
    int max = is_float ? 9 : 17;
    int p;
    for (p = 1; p <= max; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        if (is_float ? (strtof(buf, NULL) == (float)v) : (strtod(buf, NULL) == v)) break;
    }
    if (p == 1) {
        /* Java picks the closest 2-digit decimal when the shortest has 1 digit. */
        snprintf(buf, sizeof buf, "%.1e", v);
        if (!(is_float ? (strtof(buf, NULL) == (float)v) : (strtod(buf, NULL) == v))) snprintf(buf, sizeof buf, "%.0e", v);
    }
    /* buf is d[.ddd]e[+-]xx */
    int n = 0;
    const char* s = buf;
    while (*s && *s != 'e') {
        if (*s >= '0' && *s <= '9') digits[n++] = *s;
        s++;
    }
    while (n > 1 && digits[n - 1] == '0') n--;
    digits[n] = 0;
    return atoi(s + 1);
}

static void print_float(double v, int is_float) {
    if (v != v) {
        write_bytes("NaN", 3);
        return;
    }
    if (v < 0 || (v == 0 && 1 / v < 0)) {
        write_bytes("-", 1);
        v = -v;
    }
    if (v == 1.0 / 0.0) {
        write_bytes("Infinity", 8);
        return;
    }
    if (v == 0) {
        write_bytes("0.0", 3);
        return;
    }
    char d[24];
    int e = shortest(v, is_float, d);
    int n = (int)strlen(d);
    char out[64];
    int k = 0;
    if (e >= -3 && e < 7) {
        if (e < 0) {
            out[k++] = '0';
            out[k++] = '.';
            for (int i = -1; i > e; i--) out[k++] = '0';
            for (int i = 0; i < n; i++) out[k++] = d[i];
        } else {
            for (int i = 0; i <= e; i++) out[k++] = i < n ? d[i] : '0';
            out[k++] = '.';
            if (n > e + 1) {
                for (int i = e + 1; i < n; i++) out[k++] = d[i];
            } else {
                out[k++] = '0';
            }
        }
    } else {
        out[k++] = d[0];
        out[k++] = '.';
        if (n > 1) {
            for (int i = 1; i < n; i++) out[k++] = d[i];
        } else {
            out[k++] = '0';
        }
        k += snprintf(out + k, sizeof out - (size_t)k, "E%d", e);
    }
    write_bytes(out, (size_t)k);
}

void cx_print_f64(cx_f64 v) { print_float(v, 0); }
void cx_print_f32(cx_f32 v) { print_float(v, 1); }

/* --- memory and strings ------------------------------------------------------------------ */

void* cx_grow_to(void* ptr, cx_usize* cap, cx_usize need, cx_usize max, cx_usize size) {
    cx_usize n = *cap <= max / 2 ? *cap * 2 : max;
    if (n < need) n = need;
    if (n < 4 && max >= 4) n = 4;
    ptr = cx_realloc(ptr, *cap * size, n * size);
    *cap = n;
    return ptr;
}

void* cx_deque_grow(void* ptr, cx_usize* head, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    cx_usize old = *cap;
    ptr = cx_grow(ptr, cap, len, extra, size);
    cx_usize n = *cap;
    if (n == old || *head + len <= old) return ptr;
    /* The ring wrapped: `a` elements from `head` to the old end, then `b` from the start. The
       shorter run moves, the one at the start to just past the old end if it fits there. */
    cx_usize a = old - *head, b = len - a;
    char* p = (char*)ptr;
    if (b <= a && b <= n - old) {
        memcpy(p + old * size, p, b * size);
    } else {
        memmove(p + (n - a) * size, p + *head * size, a * size);
        *head = n - a;
    }
    return ptr;
}

/* --- arenas (D114) --------------------------------------------------------------------------- */

#define CX_ARENA_FIRST ((cx_usize)8192)

cx_Arena cx_arena_new(cx_usize first) {
    cx_Arena a;
    a.s = (cx_arena_state*)cx_alloc(sizeof(cx_arena_state));
    memset(a.s, 0, sizeof(cx_arena_state));
    if (first > (cx_usize)-1 / 4) cx_panic_capacity();
    a.s->first = (first ? first : CX_ARENA_FIRST) + sizeof(cx_arena_chunk);
    return a;
}

static cx_u8* cx_arena_data(cx_arena_chunk* c) { return (cx_u8*)(c + 1); }

static void cx_arena_free_chunks(cx_arena_chunk* c) {
    while (c) {
        cx_arena_chunk* prev = c->prev;
        cx_free(c, c->size);
        c = prev;
    }
}

static void cx_arena_start(cx_arena_state* s, cx_arena_chunk* c) {
    s->chunk = c;
    s->top = cx_arena_data(c);
    s->end = (cx_u8*)c + c->size;
}

void cx_drop_Arena(cx_Arena* a) {
    if (!a->s) return;
    cx_arena_free_chunks(a->s->chunk);
    cx_free(a->s, sizeof(cx_arena_state));
}

cx_Arena cx_copy_Arena(const cx_Arena* a) { return cx_arena_new(a->s->first - sizeof(cx_arena_chunk)); }

void cx_arena_reset(cx_Arena* a) {
    cx_arena_state* s = a->s;
    cx_arena_chunk* c = s->chunk;
    if (!c) return;
    if (c->prev) {
        cx_usize total = 0;
        for (cx_arena_chunk* x = c; x; x = x->prev) total += x->size;
        cx_arena_free_chunks(c);
        c = (cx_arena_chunk*)cx_alloc(total);
        c->prev = NULL;
        c->size = total;
    }
    cx_arena_start(s, c);
    s->full = 0;
#ifdef CX_DEBUG
    /* Nothing can see the memory now, so a compiler bug that let something keep it shows up. */
    memset(s->top, 0xA5, (cx_usize)(s->end - s->top));
#endif
}

cx_usize cx_arena_used(const cx_Arena* a) {
    const cx_arena_state* s = a->s;
    return s->chunk ? s->full + (cx_usize)(s->top - cx_arena_data(s->chunk)) : 0;
}

/* `bytes` from a new chunk, at least twice the size of the last: the newest has no room. */
void* cx_arena_more(cx_arena_state* s, void* ptr, cx_usize len, cx_usize bytes, cx_usize size, cx_usize align) {
    cx_usize want = s->chunk ? s->chunk->size * 2 : s->first;
    if (bytes > (cx_usize)-1 / 4) cx_panic_capacity();
    cx_usize need = sizeof(cx_arena_chunk) + bytes + align;
    if (want < need) want = need;
    cx_arena_chunk* c = (cx_arena_chunk*)cx_alloc(want);
    c->prev = s->chunk;
    c->size = want;
    if (s->chunk) s->full += (cx_usize)(s->top - cx_arena_data(s->chunk));
    cx_arena_start(s, c);
    cx_usize pad = (cx_usize)(-(uintptr_t)s->top & (align - 1));
    cx_u8* q = s->top + pad;
    s->top = q + bytes;
    if (len) memcpy(q, ptr, len * size);
    return q;
}

void cx_string_reserve(cx_String* s, cx_usize extra) {
    if (extra > (cx_usize)-1 / 2 - s->len) cx_panic_capacity();
    cx_usize need = s->len + extra;
    if (s->cap == 0) {
        /* Not owned yet: copy the literal's bytes into a buffer of its own. */
        if (need == 0) return;
        cx_usize n = need < 16 ? 16 : need;
        cx_u8* p = (cx_u8*)cx_alloc(n);
        if (s->len) memcpy(p, s->ptr, s->len);
        s->ptr = p;
        s->cap = n;
        return;
    }
    if (need <= s->cap) return;
    cx_usize n = s->cap * 2;
    if (n < need) n = need;
    s->ptr = (cx_u8*)cx_realloc(s->ptr, s->cap, n);
    s->cap = n;
}

void cx_string_push_bytes(cx_String* s, const cx_u8* p, cx_usize n) {
    if (n == 0) return;
    cx_string_reserve(s, n);
    memcpy(s->ptr + s->len, p, n);
    s->len += n;
}

void cx_string_push_rune(cx_String* s, cx_rune c) {
    cx_u8 b[4];
    cx_usize n;
    if (c < 0x80) {
        b[0] = (cx_u8)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (cx_u8)(0xC0 | (c >> 6));
        b[1] = (cx_u8)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (cx_u8)(0xE0 | (c >> 12));
        b[1] = (cx_u8)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (cx_u8)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        b[0] = (cx_u8)(0xF0 | (c >> 18));
        b[1] = (cx_u8)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (cx_u8)(0x80 | ((c >> 6) & 0x3F));
        b[3] = (cx_u8)(0x80 | (c & 0x3F));
        n = 4;
    }
    cx_string_push_bytes(s, b, n);
}

/* A copy owns its bytes, except that a literal's copy is the literal again. */
cx_String cx_copy_String(const cx_String* s) {
    if (s->cap == 0) return *s;
    cx_String r = {0, 0, 0};
    if (s->len == 0) return r;
    r.ptr = (cx_u8*)cx_alloc(s->len);
    memcpy(r.ptr, s->ptr, s->len);
    r.len = r.cap = s->len;
    return r;
}

cx_String cx_string_from(cx_str s) {
    cx_String r = {0, 0, 0};
    cx_string_push_bytes(&r, s.ptr, s.len);
    return r;
}

cx_bool cx_utf8_valid(const cx_u8* p, cx_usize n) {
    cx_usize i = 0;
    while (i < n) {
        /* Eight ASCII bytes at a time. */
        if (n - i >= 8) {
            cx_u64 w;
            memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ull)) {
                i += 8;
                continue;
            }
        }
        cx_u8 c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        cx_usize len;
        cx_u32 cp, min;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2, cp = c & 0x1F, min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, cp = c & 0x0F, min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4, cp = c & 0x07, min = 0x10000;
        } else {
            return 0;
        }
        if (n - i < len) return 0;
        for (cx_usize k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += len;
    }
    return 1;
}

/* Java's String.hashCode: h = 31 * h + c over the UTF-16 units, wrapping as an `int` does. Eight
 * ASCII bytes at a time are one step, h * 31^8 plus each byte times its power, which leaves one
 * multiply on the chain where the plain loop has eight. */
cx_i32 cx_str_java_hash(cx_str s) {
    const cx_u8* p = s.ptr;
    cx_usize n = s.len, i = 0;
    cx_u32 h = 0;
    while (i < n) {
        if (i + 8 <= n) {
            cx_u64 w;
            __builtin_memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ull)) {
                const cx_u32 p1 = 31u, p2 = p1 * 31u, p3 = p2 * 31u, p4 = p3 * 31u, p5 = p4 * 31u, p6 = p5 * 31u,
                             p7 = p6 * 31u, p8 = p7 * 31u;
                h = h * p8 + p[i] * p7 + p[i + 1] * p6 + p[i + 2] * p5 + p[i + 3] * p4 + p[i + 4] * p3 + p[i + 5] * p2 +
                    p[i + 6] * p1 + p[i + 7];
                i += 8;
                continue;
            }
        }
        if (p[i] < 0x80) {
            h = h * 31u + p[i++];
            continue;
        }
        cx_rune r = cx_utf8_next(p, &i);
        if (r < 0x10000) {
            h = h * 31u + r;
        } else {
            r -= 0x10000;
            h = h * 31u + (0xD800u | (r >> 10));
            h = h * 31u + (0xDC00u | (r & 0x3FFu));
        }
    }
    return (cx_i32)h;
}

/* Java's String.length(): a unit for each rune, and two for one past U+FFFF, which is the one whose
 * first byte is 0xF0 or more. */
cx_usize cx_str_utf16_len(cx_str s) {
    cx_usize n = 0;
    for (cx_usize i = 0; i < s.len; i++) {
        cx_u8 b = s.ptr[i];
        n += (b & 0xC0) != 0x80;
        n += b >= 0xF0;
    }
    return n;
}

/* --- parsing ------------------------------------------------------------------------------- */

/* The digits after an optional sign, up to `limit`. */
static cx_bool parse_digits(const cx_u8* p, cx_usize n, cx_u64 limit, cx_u64* out) {
    if (n == 0) return 0;
    cx_u64 v = 0;
    for (cx_usize i = 0; i < n; i++) {
        unsigned d = (unsigned)p[i] - '0';
        if (d > 9) return 0;
        if (d > limit || v > (limit - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

cx_bool cx_parse_i64(cx_str s, cx_i64 lo, cx_i64 hi, cx_i64* out) {
    *out = 0;
    if (s.len == 0) return 0;
    int neg = s.ptr[0] == '-';
    cx_usize skip = neg || s.ptr[0] == '+';
    cx_u64 v;
    if (!parse_digits(s.ptr + skip, s.len - skip, neg ? (cx_u64)0 - (cx_u64)lo : (cx_u64)hi, &v)) return 0;
    *out = neg ? (cx_i64)((cx_u64)0 - v) : (cx_i64)v;
    return 1;
}

cx_bool cx_parse_u64(cx_str s, cx_u64 hi, cx_u64* out) {
    *out = 0;
    if (s.len == 0 || s.ptr[0] == '-') return 0;
    cx_usize skip = s.ptr[0] == '+';
    return parse_digits(s.ptr + skip, s.len - skip, hi, out);
}

static int is_digit(cx_u8 c) { return c >= '0' && c <= '9'; }
static int is_hex(cx_u8 c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* Checks `s` against Java's grammar for Double.parseDouble: trimmed of characters up to ' ',
 * an optional sign, then `NaN`, `Infinity`, a decimal number with an optional exponent, or a hex
 * number with a binary exponent, then an optional `f`, `F`, `d` or `D`. Returns 0 if it doesn't
 * match, 2 for NaN and 3 for an infinity, and 1 for a number, which is copied without its suffix
 * into `buf` (of `cap` bytes) or, if longer, into memory `*heap` that the caller frees. */
static int java_float(cx_str s, char* buf, size_t cap, char** text, char** heap, int* negative) {
    const cx_u8* p = s.ptr;
    size_t n = s.len;
    while (n && p[0] <= ' ') p++, n--;
    while (n && p[n - 1] <= ' ') n--;
    if (n == 0) return 0;
    size_t i = 0;
    *negative = p[0] == '-';
    if (p[0] == '+' || p[0] == '-') i++;
    if (n - i == 3 && memcmp(p + i, "NaN", 3) == 0) return 2;
    if (n - i == 8 && memcmp(p + i, "Infinity", 8) == 0) return 3;
    size_t digits = 0;
    if (n - i >= 2 && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X')) {
        i += 2;
        while (i < n && is_hex(p[i])) i++, digits++;
        if (i < n && p[i] == '.') {
            i++;
            while (i < n && is_hex(p[i])) i++, digits++;
        }
        if (!digits || i >= n || (p[i] != 'p' && p[i] != 'P')) return 0;
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        size_t e = 0;
        while (i < n && is_digit(p[i])) i++, e++;
        if (!e) return 0;
    } else {
        while (i < n && is_digit(p[i])) i++, digits++;
        if (i < n && p[i] == '.') {
            i++;
            while (i < n && is_digit(p[i])) i++, digits++;
        }
        if (!digits) return 0;
        if (i < n && (p[i] == 'e' || p[i] == 'E')) {
            i++;
            if (i < n && (p[i] == '+' || p[i] == '-')) i++;
            size_t e = 0;
            while (i < n && is_digit(p[i])) i++, e++;
            if (!e) return 0;
        }
    }
    size_t end = i;
    if (i < n && (p[i] == 'f' || p[i] == 'F' || p[i] == 'd' || p[i] == 'D')) i++;
    if (i != n) return 0;
    char* t = buf;
    if (end + 1 > cap) t = *heap = (char*)malloc(end + 1);
    if (!t) cx_panic_capacity();
    memcpy(t, p, end);
    t[end] = 0;
    *text = t;
    return 1;
}

cx_bool cx_parse_f64(cx_str s, cx_f64* out) {
    char buf[128], *text = 0, *heap = 0;
    int negative;
    *out = 0;
    switch (java_float(s, buf, sizeof buf, &text, &heap, &negative)) {
        case 0: return 0;
        case 2: *out = __builtin_nan(""); return 1;
        case 3: *out = negative ? -__builtin_inf() : __builtin_inf(); return 1;
    }
    *out = strtod(text, 0);
    free(heap);
    return 1;
}

cx_bool cx_parse_f32(cx_str s, cx_f32* out) {
    char buf[128], *text = 0, *heap = 0;
    int negative;
    *out = 0;
    switch (java_float(s, buf, sizeof buf, &text, &heap, &negative)) {
        case 0: return 0;
        case 2: *out = __builtin_nanf(""); return 1;
        case 3: *out = negative ? -__builtin_inff() : __builtin_inff(); return 1;
    }
    /* Straight to f32, rounding once, as Float.parseFloat does. */
    *out = strtof(text, 0);
    free(heap);
    return 1;
}

cx_bool cx_str_contains(cx_str hay, cx_str needle) { return cx_str_find(hay, needle) != CX_NOT_FOUND; }

/* `memchr` finds each place the needle's first byte is, which it does many bytes a step, and only
 * there are the rest compared. */
cx_usize cx_str_find(cx_str hay, cx_str needle) {
    if (needle.len == 0) return 0;
    if (needle.len > hay.len) return CX_NOT_FOUND;
    const cx_u8* p = hay.ptr;
    const cx_u8* stop = hay.ptr + (hay.len - needle.len) + 1;  // past the last place it can start
    cx_u8 first = needle.ptr[0];
    while (p < stop) {
        p = (const cx_u8*)memchr(p, first, (size_t)(stop - p));
        if (!p) return CX_NOT_FOUND;
        if (memcmp(p + 1, needle.ptr + 1, needle.len - 1) == 0) return (cx_usize)(p - hay.ptr);
        p++;
    }
    return CX_NOT_FOUND;
}

cx_usize cx_str_find_last(cx_str hay, cx_str needle) {
    if (needle.len > hay.len) return CX_NOT_FOUND;
    if (needle.len == 0) return hay.len;
    cx_u8 first = needle.ptr[0];
    for (cx_usize i = hay.len - needle.len + 1; i-- > 0;) {
        if (hay.ptr[i] == first && memcmp(hay.ptr + i + 1, needle.ptr + 1, needle.len - 1) == 0) return i;
    }
    return CX_NOT_FOUND;
}

cx_String* cx_fmt_begin(cx_String* s) {
    cx_String* previous = fmt_to;
    fmt_to = s;
    return previous;
}

void cx_fmt_end(cx_String* previous) { fmt_to = previous; }

/* --- panics --------------------------------------------------------------------------------
 * The message goes through the print functions' path, so inside a `parallel for` it's kept
 * with the rest of its chunk's output. */

static CX_COLD void par_panicked(void);

void cx_panic_begin(const char* at) {
    fmt_to = 0;
    /* Outside a chunk, this thread ends the program, unless another is already ending it. */
    if (!par_buffered) {
        take_exit();
        flush_out();
    }
    to_stderr = 1;
    panic_text("panic at ");
    panic_text(at);
    panic_text(": ");
}

void cx_panic_end(void) {
    panic_text("\n");
    cx_trace_write(panic_text, par_buffered ? 1 : 0, 0);
    if (par_buffered) par_panicked();
    fflush(stderr);
    exit(101);
}

void cx_panic_index(cx_usize index, cx_usize length, const char* at) {
    char text[96];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "index %llu is out of bounds for length %llu", (unsigned long long)index,
             (unsigned long long)length);
    panic_text(text);
    cx_panic_end();
}

void cx_panic_slice(cx_str s, cx_usize lo, cx_usize hi, cx_bool inclusive, const char* at) {
    char text[160];
    cx_usize end = hi + (inclusive ? 1 : 0);
    const char* dots = inclusive ? "..=" : "..";
    unsigned long long a = (unsigned long long)lo, b = (unsigned long long)hi, n = (unsigned long long)s.len;
    cx_panic_begin(at);
    if (end > s.len || end < hi) {
        snprintf(text, sizeof text, "the range %llu%s%llu is out of bounds for text of length %llu", a, dots, b, n);
    } else if (lo > end) {
        snprintf(text, sizeof text, "the range %llu%s%llu starts after it ends", a, dots, b);
    } else {
        cx_usize inside = cx_utf8_starts_at(s, lo) ? end : lo;
        snprintf(text, sizeof text, "the range %llu%s%llu cuts a rune apart: byte %llu is inside one, in text of length %llu", a,
                 dots, b, (unsigned long long)inside, n);
    }
    panic_text(text);
    cx_panic_end();
}

void cx_panic_div_zero(const char* at) {
    cx_panic_begin(at);
    panic_text("division by zero");
    cx_panic_end();
}

void cx_panic_capacity(void) {
    cx_panic_begin("<memory>");
    panic_text("out of memory");
    cx_panic_end();
}

void cx_panic_order(const char* at) {
    cx_panic_begin(at);
    panic_text("`sort` found an `Order` that contradicts itself, such as one where a < b and b < a");
    cx_panic_end();
}

void cx_panic_insert(cx_usize index, cx_usize length, const char* at) {
    char text[96];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "can't insert at index %llu in a list of length %llu", (unsigned long long)index,
             (unsigned long long)length);
    panic_text(text);
    cx_panic_end();
}

void cx_panic_set_len(cx_usize length, cx_usize capacity, const char* at) {
    char text[96];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "can't set the length to %llu in a list with capacity %llu", (unsigned long long)length,
             (unsigned long long)capacity);
    panic_text(text);
    cx_panic_end();
}

void cx_panic_unwrap(const char* at) {
    cx_panic_begin(at);
    panic_text("unwrapped `none` with `!`");
    cx_panic_end();
}

void cx_panic_unwrap_err(const char* at) {
    cx_panic_begin(at);
    panic_text("unwrapped an error with `!`");
    cx_panic_end();
}

void cx_panic_key(const char* at) {
    cx_panic_begin(at);
    panic_text("the key isn't in the map; add it with `insert`");
    cx_panic_end();
}

void cx_panic_marks(cx_usize marks, cx_usize length, const char* at) {
    char text[112];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "`remove_marked` needs a mark for each element: %llu marks for %llu elements",
             (unsigned long long)marks, (unsigned long long)length);
    panic_text(text);
    cx_panic_end();
}

void cx_panic_range(const char* at) {
    cx_panic_begin(at);
    panic_text("the range ends before it starts");
    cx_panic_end();
}

void cx_panic_load_factor(const char* at) {
    cx_panic_begin(at);
    panic_text("the load factor must be greater than 0 and less than 1");
    cx_panic_end();
}

void cx_panic_table_size(const char* at) {
    cx_panic_begin(at);
    panic_text("more keys than fastutil's tables hold, which is 2^30 slots at the load factor");
    cx_panic_end();
}

/* --- hashing and tables (D94) ---------------------------------------------------------------- */

static cx_u64 read_u64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return v;
}
static cx_u64 read_u32(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}

/* 16 bytes at a time, each pair of words folded into the state. The last 16, or the whole of a
 * shorter text, are read as two words that may overlap, so no byte is read twice in a way that
 * depends on where the text sits in memory. Reads are little-endian on every target Calx has. */
cx_u64 cx_hash_bytes(const cx_u8* p, cx_usize n) {
    cx_u64 h = CX_HASH_SEED ^ ((cx_u64)n * CX_HASH_MUL);
    cx_u64 a, b;
    if (n <= 16) {
        if (n >= 8) {
            a = read_u64(p);
            b = read_u64(p + n - 8);
        } else if (n >= 4) {
            a = read_u32(p);
            b = read_u32(p + n - 4);
        } else if (n > 0) {
            a = ((cx_u64)p[0] << 16) | ((cx_u64)p[n >> 1] << 8) | p[n - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        while (n > 16) {
            h = cx_hash_mix(read_u64(p) ^ 0xA0761D6478BD642Full, read_u64(p + 8) ^ h);
            p += 16;
            n -= 16;
        }
        a = read_u64(p + n - 16);
        b = read_u64(p + n - 8);
    }
    return cx_hash_mix(a ^ 0xE7037ED1A0B428DBull, b ^ h);
}

const cx_u8 cx_table_empty[CX_GROUP] = {
    CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY,
    CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY, CX_CTRL_EMPTY,
};

cx_usize cx_table_buckets_for(cx_usize n) {
    if (n < 4) return 4;
    if (n < 8) return 8;
    if (n > ((cx_usize)-1 >> 4)) cx_panic_capacity();
    cx_usize want = n * 8 / 7 + 1, b = 16;
    while (b < want) b *= 2;
    return b;
}

/* A table's slots, rounded up to a group, then its control bytes and their copy. */
static cx_usize table_slot_bytes(cx_usize buckets, cx_usize size) { return (buckets * size + 15) & ~(cx_usize)15; }

cx_u8* cx_table_alloc(cx_usize buckets, cx_usize size, void** slots) {
    if (buckets > ((cx_usize)-1 >> 1) / (size + 1)) cx_panic_capacity();
    cx_usize bytes = table_slot_bytes(buckets, size);
    cx_u8* base = (cx_u8*)cx_alloc(bytes + buckets + CX_GROUP);
    memset(base + bytes, CX_CTRL_EMPTY, buckets + CX_GROUP);
    *slots = base;
    return base + bytes;
}

void cx_table_free(void* slots, cx_usize buckets, cx_usize size) {
    cx_free(slots, table_slot_bytes(buckets, size) + buckets + CX_GROUP);
}

cx_u32* cx_lk_alloc(cx_usize buckets) {
    if (buckets > (cx_usize)0xFFFFFFFFu) cx_panic_capacity();
    return (cx_u32*)cx_alloc(buckets * 2 * sizeof(cx_u32));
}

void cx_lk_free(cx_u32* link, cx_usize buckets) { cx_free(link, buckets * 2 * sizeof(cx_u32)); }

/* --- parallel for ---------------------------------------------------------------------------
 * `parallel for` runs its body on a pool of threads that the calling thread takes part in. The
 * iterations are split into chunks, which threads claim in order from a counter. The checker
 * makes sure no two iterations touch the same memory (D21), so what they compute can't depend
 * on which thread ran what. What they print is kept per chunk and written in chunk order when
 * the loop ends, and a panic reports the first iteration that panicked, after the output of
 * every iteration before it. So the output is the same with one thread and with many, too.
 *
 * The outermost loop is the top job, which the main thread opens. Workers wait for it on its
 * word, which is odd while it's open: they spin for a while, since loops often come one after
 * another, then sleep on it. A thread joins a job by counting itself in the job's `active` and
 * then checking that the word hasn't changed. The job's owner closes it once every chunk is
 * claimed and waits for `active` to reach 0, so no thread reads a job after that, and a
 * sleeping thread is never waited for.
 *
 * A loop inside a chunk is an inner job, in one of `PAR_NESTED` slots. A thread with no chunks
 * of the top job left to claim stays in it and helps inner jobs, until every chunk of the top
 * job has finished, so a region that runs a loop of its own gets the threads the other regions
 * aren't using. An inner job's output goes into the chunk that opened it, in chunk order, and a
 * panic in it is a panic in that chunk, once the chunks before the one that panicked are done.
 * With every slot taken, a loop runs on the thread that reaches it. */

#define PAR_NONE (~(cx_u64)0)
#define PAR_SPIN_NS 200000u /* how long a thread spins before it sleeps */
#define PAR_CHUNKS 8        /* chunks per thread, so a slow chunk doesn't hold the rest up */
#define PAR_NESTED 64       /* inner jobs open at once, one bit each of `par_open` */
#define PAR_SHARED __attribute__((aligned(128))) /* its own pair of cache lines */

typedef void (*par_body)(void* ctx, cx_u64 begin, cx_u64 end);

/* A chunk's output: segments of one byte naming the stream (1 for standard error), the length,
 * and the bytes. */
typedef struct {
    unsigned char* p;
    size_t len, cap;
    size_t last; /* where the last segment starts, or SIZE_MAX */
} par_out;

/* A loop's chunks. Its owner writes the first part before opening it, and nothing changes that
 * while it's open. */
typedef struct par_job {
    par_body body;
    void* ctx;
    cx_u64 n, chunk, chunks;
    unsigned char* done; /* per chunk: whether it has run, for a panic */
    par_out** outs;      /* per chunk: what it printed, once it prints */
    cx_u64 cap;          /* the length of both */
    /* An inner job: the chunk its owner was running when it opened it, where its output goes. */
    struct par_job* up;
    cx_u64 up_chunk;
    int up_owner;
    unsigned slot;
    PAR_SHARED cx_u32 word;   /* jobs opened * 2, plus 1 while one is open */
    PAR_SHARED cx_u64 next;   /* the next chunk to claim */
    PAR_SHARED cx_u32 active; /* threads in it besides its owner */
    PAR_SHARED cx_u64 panic;  /* the first chunk that panicked */
    PAR_SHARED cx_u64 left;   /* the top job: chunks that haven't finished */
} par_job;

static par_job par_top = {.panic = PAR_NONE};
static par_job par_nest[PAR_NESTED];
static PAR_SHARED cx_u64 par_taken;          /* bit k: a thread owns `par_nest[k]` */
static PAR_SHARED cx_u64 par_open;           /* bit k: `par_nest[k]` is open */
static PAR_SHARED cx_u32 par_sleepers;       /* workers asleep on the top job's word */
static PAR_SHARED cx_u32 par_help_word;      /* changes when an inner job opens or the top job's chunks finish */
static PAR_SHARED cx_u32 par_help_sleepers;  /* threads asleep on it */
static cx_usize par_target;  /* threads to use, the main thread included; 0 until set or read */
static cx_usize par_started; /* worker threads started */

static _Thread_local par_job* par_cur; /* the job whose chunk this thread is running */
static _Thread_local cx_u64 par_chunk; /* that chunk */
static _Thread_local int par_owner;    /* this thread owns `par_cur` */
static _Thread_local int par_worker;   /* this is a worker thread */
static _Thread_local int par_ending;   /* the inner jobs this thread is ending a panic in */

static void par_pause(void) { __builtin_ia32_pause(); }

#ifdef _WIN32
static void par_sleep(cx_u32* word, cx_u32 seen) { WaitOnAddress(word, &seen, sizeof seen, INFINITE); }
static void par_wake(cx_u32* word) { WakeByAddressAll(word); }
static void par_yield(void) { SwitchToThread(); }
#else
static void par_sleep(cx_u32* word, cx_u32 seen) { syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, seen, NULL, NULL, 0); }
static void par_wake(cx_u32* word) { syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0); }
static void par_yield(void) { sched_yield(); }
#endif

static CX_COLD void par_fail(const char* what) {
    take_exit();
    fputs(what, stderr);
    fflush(stderr);
    _Exit(101);
}

static void par_write(int stream, const void* p, size_t n) {
    par_out** slot = &par_cur->outs[par_chunk];
    par_out* o = *slot;
    if (!o) {
        o = calloc(1, sizeof *o);
        if (!o) par_fail("panic at <memory>: out of memory\n");
        o->last = SIZE_MAX;
        *slot = o;
    }
    size_t need = o->len + 1 + sizeof(size_t) + n;
    if (need > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        if (cap < need) cap = need;
        unsigned char* q = realloc(o->p, cap);
        if (!q) par_fail("panic at <memory>: out of memory\n");
        o->p = q;
        o->cap = cap;
    }
    size_t len;
    if (o->last != SIZE_MAX && o->p[o->last] == stream) {
        memcpy(&len, o->p + o->last + 1, sizeof len);
        len += n;
        memcpy(o->p + o->last + 1, &len, sizeof len);
    } else {
        o->last = o->len;
        o->p[o->len] = (unsigned char)stream;
        len = n;
        memcpy(o->p + o->len + 1, &len, sizeof len);
        o->len += 1 + sizeof len;
    }
    memcpy(o->p + o->len, p, n);
    o->len += n;
}

/* Writes what chunks [0, upto) of `j` printed, in order, as if they'd printed it themselves:
 * into the chunk running on this thread, or out. */
static void par_replay(par_job* j, cx_u64 upto) {
    for (cx_u64 c = 0; c < upto; c++) {
        par_out* o = j->outs[c];
        if (!o) continue;
        for (size_t at = 0; at < o->len;) {
            int stream = o->p[at];
            size_t len;
            memcpy(&len, o->p + at + 1, sizeof len);
            at += 1 + sizeof len;
            if (par_cur) {
                par_write(stream, o->p + at, len);
            } else if (stream) {
                flush_out();
                err_bytes(o->p + at, len);
            } else {
                out_bytes(o->p + at, len);
            }
            at += len;
        }
        free(o->p);
        free(o);
        j->outs[c] = 0;
    }
}

/* Room for `chunks` chunks in `j`, none run and none printed. Only its owner calls this. */
static void par_size(par_job* j, cx_u64 chunks) {
    if (chunks > j->cap) {
        free(j->done);
        free(j->outs);
        j->done = malloc(chunks);
        j->outs = calloc(chunks, sizeof *j->outs);
        if (!j->done || !j->outs) par_fail("panic at <memory>: out of memory\n");
        j->cap = chunks;
    }
    memset(j->done, 0, chunks);
}

/* Closes `j`, if it's open, so no thread joins it from now on. */
static void par_close(par_job* j) {
    cx_u32 w = __atomic_load_n(&j->word, __ATOMIC_RELAXED);
    if (w & 1) __atomic_store_n(&j->word, w + 1, __ATOMIC_SEQ_CST);
}

/* Wakes the threads helping inner jobs: one has opened, or the top job's chunks have finished. */
static void par_nudge(void) {
    __atomic_fetch_add(&par_help_word, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&par_help_sleepers, __ATOMIC_SEQ_CST)) par_wake(&par_help_word);
}

/* Claims chunks of `j` and runs them until none are left, as its owner or as a helper. Never
 * inlined, so a stack trace tells a chunk's body from a loop that runs where it is. */
static __attribute__((noinline)) void par_claim(par_job* j, int owner) {
    par_job* was = par_cur;
    cx_u64 was_chunk = par_chunk;
    int was_owner = par_owner;
    par_cur = j;
    par_owner = owner;
    par_buffered = 1;
    for (;;) {
        if (__atomic_load_n(&j->panic, __ATOMIC_RELAXED) != PAR_NONE) break;
        cx_u64 c = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        if (c >= j->chunks) break;
        par_chunk = c;
        cx_u64 begin = c * j->chunk;
        cx_u64 end = j->n - begin < j->chunk ? j->n : begin + j->chunk;
        j->body(j->ctx, begin, end);
        __atomic_store_n(&j->done[c], 1, __ATOMIC_RELEASE);
        if (j == &par_top && __atomic_sub_fetch(&j->left, 1, __ATOMIC_ACQ_REL) == 0) par_nudge();
    }
    par_cur = was;
    par_chunk = was_chunk;
    par_owner = was_owner;
    par_buffered = was != 0;
}

/* Runs chunks of every open inner job once. Returns whether there were any. */
static int par_help_once(void) {
    cx_u64 open = __atomic_load_n(&par_open, __ATOMIC_ACQUIRE);
    int helped = 0;
    while (open) {
        par_job* j = &par_nest[__builtin_ctzll(open)];
        open &= open - 1;
        cx_u32 w = __atomic_load_n(&j->word, __ATOMIC_SEQ_CST);
        if (!(w & 1)) continue;
        __atomic_fetch_add(&j->active, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&j->word, __ATOMIC_SEQ_CST) == w) {
            par_claim(j, 0);
            helped = 1;
        }
        __atomic_fetch_sub(&j->active, 1, __ATOMIC_RELEASE);
    }
    return helped;
}

/* On a worker whose chunks of the top job are claimed: helps inner jobs until every chunk of
 * the top job has finished, sleeping when there's been nothing to help for a while. */
static void par_help(void) {
    cx_u64 start = cx_clock_ns();
    for (unsigned spins = 1;; spins++) {
        if (!__atomic_load_n(&par_top.left, __ATOMIC_ACQUIRE)) return;
        if (__atomic_load_n(&par_top.panic, __ATOMIC_RELAXED) != PAR_NONE) return;
        cx_u32 seen = __atomic_load_n(&par_help_word, __ATOMIC_SEQ_CST);
        if (par_help_once()) {
            start = cx_clock_ns();
            continue;
        }
        par_pause();
        if (spins % 64 || cx_clock_ns() - start <= PAR_SPIN_NS) continue;
        __atomic_fetch_add(&par_help_sleepers, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&par_help_word, __ATOMIC_SEQ_CST) == seen && __atomic_load_n(&par_top.left, __ATOMIC_SEQ_CST)) {
            par_sleep(&par_help_word, seen);
        }
        __atomic_fetch_sub(&par_help_sleepers, 1, __ATOMIC_RELAXED);
        start = cx_clock_ns();
    }
}

/* Waits until no thread but its owner is in `j`, which its owner has closed. */
static void par_drain(par_job* j) {
    for (unsigned spins = 0; __atomic_load_n(&j->active, __ATOMIC_SEQ_CST); spins++) {
        if (spins < 4096) par_pause();
        else par_yield();
    }
}

/* After a panic in the top job, on the main thread: once every chunk before the first one that
 * panicked has run, writes their output and that chunk's, which ends with the panic message and
 * the frames inside the loop, then the frames outside it, and exits. */
static CX_COLD void par_end_panic(void) {
    take_exit();
    par_close(&par_top);
    cx_u64 first;
    for (unsigned spins = 0;; spins++) {
        first = __atomic_load_n(&par_top.panic, __ATOMIC_ACQUIRE);
        cx_u64 c = 0;
        while (c < first && __atomic_load_n(&par_top.done[c], __ATOMIC_ACQUIRE)) c++;
        if (c == first) break;
        if (spins < 4096) par_pause();
        else par_yield();
    }
    par_cur = 0;
    par_buffered = 0;
    to_stderr = 0;
    par_replay(&par_top, first + 1);
    flush_out();
    fmt_to = 0;
    to_stderr = 1;
    cx_trace_write(panic_text, 2, 0);
    fflush(stderr);
    _Exit(101);
}

static CX_COLD void par_panicked(void);

/* After a panic in the inner job `j`, on its owner: once its helpers have left, so every chunk
 * before the one that panicked has run, writes their output and that chunk's into the chunk
 * that opened `j`, and panics there. */
static CX_COLD void par_end_inner(par_job* j) {
    par_close(j);
    __atomic_fetch_and(&par_open, ~((cx_u64)1 << j->slot), __ATOMIC_RELEASE);
    par_drain(j);
    par_cur = j->up;
    par_chunk = j->up_chunk;
    par_owner = j->up_owner;
    par_buffered = 1;
    par_replay(j, __atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) + 1);
    /* None of these return, so each inner loop this thread has ended a panic in is on its stack. */
    fmt_to = 0;
    to_stderr = 1;
    cx_trace_write(panic_text, 3, ++par_ending);
    __atomic_fetch_and(&par_taken, ~((cx_u64)1 << j->slot), __ATOMIC_RELEASE);
    par_panicked();
}

static CX_COLD void par_panicked(void) {
    par_job* j = par_cur;
    cx_u64 c = par_chunk;
    cx_u64 first = __atomic_load_n(&j->panic, __ATOMIC_ACQUIRE);
    while (c < first && !__atomic_compare_exchange_n(&j->panic, &first, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (par_owner) {
        if (j == &par_top) par_end_panic();
        par_end_inner(j);
    }
    /* A helper leaves the job, and its owner takes the panic from there. */
    __atomic_fetch_sub(&j->active, 1, __ATOMIC_RELEASE);
    if (!par_worker) {
        /* The main thread, helping an inner job: the panic reaches the top job through the
         * inner job's owner, and the main thread ends the program then. */
        while (__atomic_load_n(&par_top.panic, __ATOMIC_ACQUIRE) == PAR_NONE) par_yield();
        par_end_panic();
    }
    /* A worker stops for good; the main thread ends the program. */
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

/* Waits until the top job's word isn't `seen`. */
static void par_wait(cx_u32 seen) {
    cx_u64 start = cx_clock_ns();
    for (unsigned spins = 1;; spins++) {
        if (__atomic_load_n(&par_top.word, __ATOMIC_ACQUIRE) != seen) return;
        par_pause();
        if (spins % 64 == 0 && cx_clock_ns() - start > PAR_SPIN_NS) break;
    }
    __atomic_fetch_add(&par_sleepers, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&par_top.word, __ATOMIC_SEQ_CST) == seen) par_sleep(&par_top.word, seen);
    __atomic_fetch_sub(&par_sleepers, 1, __ATOMIC_RELAXED);
}

static void par_work(cx_usize id) {
    par_worker = 1;
    cx_u32 seen = 0;
    for (;;) {
        par_wait(seen);
        cx_u32 w = __atomic_load_n(&par_top.word, __ATOMIC_SEQ_CST);
        seen = w;
        if (!(w & 1)) continue; /* a job that closed before this worker saw it */
        __atomic_fetch_add(&par_top.active, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&par_top.word, __ATOMIC_SEQ_CST) == w && id + 1 < par_target) {
            par_depth = 1;
            par_claim(&par_top, 0);
            par_help();
            par_depth = 0;
        }
        __atomic_fetch_sub(&par_top.active, 1, __ATOMIC_RELEASE);
    }
}

#ifdef _WIN32
static DWORD WINAPI par_thread(LPVOID id) {
    par_work((cx_usize)(uintptr_t)id);
    return 0;
}
#else
static void* par_thread(void* id) {
    par_work((cx_usize)(uintptr_t)id);
    return 0;
}
#endif

static cx_usize par_threads(void) {
    if (!par_target) {
#ifdef _WIN32
        long n = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        cpu_set_t set;
        long n = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : sysconf(_SC_NPROCESSORS_ONLN);
#endif
        par_target = n > 0 ? (cx_usize)n : 1;
    }
    return par_target;
}

/* For the parts of the runtime in other files, such as calx_net.c. */
int cx_in_parallel(void) { return par_depth; }

void cx_panic_text(const char* at, const char* message) {
    cx_panic_begin(at);
    panic_text(message);
    cx_panic_end();
}

/* Declared by std/pool.calx. */
cx_usize cx_workers(void) { return par_threads(); }

/* Declared by std/pool.calx, which checks that `n` isn't 0. */
void cx_set_workers(cx_usize n) {
    if (par_depth) {
        cx_panic_begin("std.pool.set_workers");
        panic_text("can't change the number of workers inside a `parallel for`");
        cx_panic_end();
    }
    if (par_service) {
        cx_panic_begin("std.pool.set_workers");
        panic_text("can't change the number of workers on a thread `Thread.spawn` started, whose loops run on it alone");
        cx_panic_end();
    }
    par_target = n;
}

static void par_start_workers(cx_usize threads) {
    while (par_started + 1 < threads) {
        uintptr_t id = par_started;
#ifdef _WIN32
        HANDLE h;
        if (cx_os_thread(&h, par_thread, (LPVOID)id)) par_fail("panic at <parallel>: can't start a worker thread\n");
        CloseHandle(h);
#else
        pthread_t t;
        if (cx_os_thread(&t, par_thread, (void*)id)) par_fail("panic at <parallel>: can't start a worker thread\n");
        pthread_detach(t);
#endif
        par_started++;
    }
}

/* Sets up `j` for `n` iterations of `body`, as its owner, on `threads` threads. */
static void par_setup(par_job* j, cx_u64 n, par_body body, void* ctx, cx_usize threads) {
    cx_u64 chunk = n / ((cx_u64)threads * PAR_CHUNKS);
    if (chunk == 0) chunk = 1;
    cx_u64 chunks = n / chunk + (n % chunk != 0);
    par_size(j, chunks);
    j->body = body;
    j->ctx = ctx;
    j->n = n;
    j->chunk = chunk;
    j->chunks = chunks;
    j->panic = PAR_NONE;
    __atomic_store_n(&j->next, 0, __ATOMIC_RELAXED);
}

/* A loop inside a chunk, on a slot of its own that idle threads help with. Returns 0 if every
 * slot is taken. Never inlined, so a stack trace finds where the loop starts. */
static __attribute__((noinline)) int par_inner(cx_u64 n, par_body body, void* ctx, cx_usize threads) {
    cx_u64 taken = __atomic_load_n(&par_taken, __ATOMIC_RELAXED);
    unsigned k;
    do {
        if (!~taken) return 0;
        k = (unsigned)__builtin_ctzll(~taken);
    } while (!__atomic_compare_exchange_n(&par_taken, &taken, taken | ((cx_u64)1 << k), 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    par_job* j = &par_nest[k];
    par_setup(j, n, body, ctx, threads);
    j->slot = k;
    j->up = par_cur;
    j->up_chunk = par_chunk;
    j->up_owner = par_owner;
    __atomic_store_n(&j->word, j->word + 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&par_open, (cx_u64)1 << k, __ATOMIC_RELEASE);
    par_nudge();

    par_claim(j, 1);
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_inner(j);
    par_close(j);
    __atomic_fetch_and(&par_open, ~((cx_u64)1 << k), __ATOMIC_RELEASE);
    par_drain(j);
    /* A helper's chunk may have panicked after the owner's last check. */
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_inner(j);
    par_replay(j, j->chunks);
    __atomic_fetch_and(&par_taken, ~((cx_u64)1 << k), __ATOMIC_RELEASE);
    return 1;
}

/* Never inlined, so a stack trace finds where a loop starts. */
__attribute__((noinline)) void cx_parallel(cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx) {
    if (n == 0) return;
    /* The pool is the main thread's, so a thread `Thread.spawn` started runs its loops alone. */
    cx_usize threads = par_service ? 1 : par_threads();
    if (threads <= 1 || n == 1 || (par_cur && !par_inner(n, body, ctx, threads))) {
        par_depth++;
        body(ctx, 0, n);
        par_depth--;
        return;
    }
    if (par_cur) return;
    par_start_workers(threads);
    par_job* j = &par_top;
    par_setup(j, n, body, ctx, threads);
    j->up = 0;
    __atomic_store_n(&j->left, j->chunks, __ATOMIC_RELAXED);
    __atomic_store_n(&j->word, j->word + 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&par_sleepers, __ATOMIC_SEQ_CST)) par_wake(&j->word);

    int depth = par_depth;
    par_depth = depth + 1;
    par_claim(j, 1);
    /* Helps the chunks still running with their inner loops, until they finish. */
    for (unsigned spins = 0; __atomic_load_n(&j->left, __ATOMIC_ACQUIRE); spins++) {
        if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic();
        if (par_help_once()) spins = 0;
        else if (spins < 4096) par_pause();
        else par_yield();
    }
    par_close(j);
    for (unsigned spins = 0; __atomic_load_n(&j->active, __ATOMIC_SEQ_CST); spins++) {
        if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic();
        if (spins < 4096) par_pause();
        else par_yield();
    }
    par_depth = depth;
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic();
    par_replay(j, j->chunks);
}
