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
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#ifdef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h> /* K32GetProcessMemoryInfo, which kernel32 has */
/* WaitOnAddress and WakeByAddressAll, for the workers of `parallel for`. */
#pragma comment(lib, "synchronization.lib")
#else
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <termios.h>
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

static void term_end(int flush);

/* Set by calx_proc.c once the program has started another, and called as the program ends, with
 * whether it's ending as it means to: by returning from `main` or calling `exit`. When it isn't,
 * as after a panic, the programs it started end with it. */
void (*cx_children_at_exit)(int normal);

/* The terminal is put back as the program found it first (see "std.term" below), so what the
 * program writes as it ends, such as a panic's message, shows as it would have. `normal` says
 * whether it's ending as it means to. */
static void take_exit_as(int normal) {
    if (exit_mine) return;
    if (__atomic_exchange_n(&exit_taken, 1, __ATOMIC_ACQ_REL)) park();
    exit_mine = 1;
    void (*children)(int) = __atomic_load_n(&cx_children_at_exit, __ATOMIC_ACQUIRE);
    if (children) children(normal);
    term_end(1);
}

static void take_exit(void) { take_exit_as(0); }

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
static arg_text wide_text(const wchar_t* w, int n) {
    int bytes = n ? WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL) : 0;
    arg_text a;
    a.ptr = must_alloc((size_t)bytes);
    a.len = (cx_usize)(bytes ? WideCharToMultiByte(CP_UTF8, 0, w, n, (char*)a.ptr, bytes, NULL, NULL) : 0);
    return a;
}

static void add_wide_arg(const wchar_t* w, int n) { args[arg_count++] = wide_text(w, n); }

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
static arg_text byte_text(const char* text) {
    const cx_u8* s = (const cx_u8*)text;
    size_t n = strlen(text);
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
    return (arg_text){out, len};
}

static void read_args(int argc, char** argv) {
    args = must_alloc((size_t)(argc > 0 ? argc : 1) * sizeof(arg_text));
    for (int i = 0; i < argc; i++) args[arg_count++] = byte_text(argv[i]);
}
#endif

/* `calx run` puts the full path of the file it built the program from in CALX_SCRIPT, which is
 * taken out of the environment here, before anything else runs, so a program this one starts
 * doesn't see it, unless `calx run` started that one too and gave it its own. */
static arg_text script;

static void take_script(void) {
#ifdef _WIN32
    DWORD size = GetEnvironmentVariableW(L"CALX_SCRIPT", NULL, 0);
    if (size == 0) return;
    wchar_t* w = must_alloc((size_t)size * sizeof(wchar_t));
    DWORD len = GetEnvironmentVariableW(L"CALX_SCRIPT", w, size);
    if (len > 0 && len < size) script = wide_text(w, (int)len);
    free(w);
    SetEnvironmentVariableW(L"CALX_SCRIPT", NULL);
#else
    const char* s = getenv("CALX_SCRIPT");
    if (!s) return;
    if (*s) script = byte_text(s);
    unsetenv("CALX_SCRIPT");
#endif
}

/* Declared by std/env.calx: the script's path, with no bytes when there's none. */
const cx_u8* cx_script(void) { return script.ptr; }
cx_usize cx_script_len(void) { return script.len; }

/* A test program's start, for `calx test` (CLX-87): `cx_start`, then the number of the test to
 * run, which `calx test` gives as the first argument, `--calx-test=N`. That's taken out of the
 * arguments, so a test sees them as a program run by itself would. -1 when there's none, and the
 * program lists its tests. */
cx_i64 cx_test_start(int argc, char** argv) {
    cx_start(argc, argv);
    static const char flag[] = "--calx-test=";
    const cx_usize n = sizeof flag - 1;
    if (arg_count < 2 || args[1].len <= n || memcmp(args[1].ptr, flag, n) != 0) return -1;
    cx_i64 which = 0;
    for (cx_usize i = n; i < args[1].len; i++) {
        cx_u8 c = args[1].ptr[i];
        if (c < '0' || c > '9' || which > 100000000) return -1;
        which = which * 10 + (c - '0');
    }
    memmove(&args[1], &args[2], (arg_count - 2) * sizeof(arg_text));
    arg_count--;
    return which;
}

/* Declared by std/env.calx. Argument 0 is the program's name. */
cx_usize cx_arg_count(void) { return arg_count; }
const cx_u8* cx_arg(cx_usize i) { return args[i].ptr; }
cx_usize cx_arg_len(cx_usize i) { return args[i].len; }

/* Declared by std/env.calx: the system the program was built for, 1 for Windows, 2 for Linux and
   3 for macOS, and its processor, 1 for x86-64 and 2 for Arm64 (D612). */
cx_u32 cx_env_os(void) {
#if defined(_WIN32)
    return 1;
#elif defined(__APPLE__)
    return 3;
#else
    return 2;
#endif
}
cx_u32 cx_env_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return 2;
#else
    return 1;
#endif
}

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
    take_script();
    overflow_start();
}

int cx_finish(void) {
    take_exit_as(1);
    flush_out();
#ifdef CX_DEBUG
    /* Debug builds count the blocks still allocated, and a program that ends with any left has
     * leaked: the compiler drops every value that owns memory (D76), so that's a compiler bug.
     * A thread still running owns what it holds, and stops where it is, so then there's no check. */
    cx_usize live = cx_alloc_live();
    if (live && !__atomic_load_n(&services_live, __ATOMIC_ACQUIRE)) {
        fprintf(stderr, "calx: %llu allocation%s never freed; this is a bug in calx, please report it\n",
                (unsigned long long)live, live == 1 ? " was" : "s were");
        /* With CALX_LEAKS=1, where each was allocated (CLX-261). */
        if (!cx_alloc_report()) fputs("calx: run it with CALX_LEAKS=1 to see where each was allocated\n", stderr);
        fflush(stderr);
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
 * buffer is written first, since the program may be waiting for an answer to it. Standard input
 * that isn't open, as a service's or a detached process's may not be, reads as empty. */
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
    if (r < 0 && errno == EBADF) return 0;
    if (r < 0) {
        cx_panic_begin("<stdin>");
        panic_text("can't read standard input");
        cx_panic_end();
    }
    return (cx_usize)r;
}

/* --- std.term: the terminal ----------------------------------------------------------------
 * Declared by std/term.calx as C functions, not in calx_rt.h. A program that puts standard input
 * in raw mode, or draws on the terminal, has it put back however it ends: `term_end` runs from
 * `take_exit`, so as `main` returns, at a panic and at `exit`, and also at the signals that end a
 * program from outside on POSIX, and at the console's Ctrl+C, Ctrl+Break and close on Windows. */

/* What's written to standard output at the end, to undo what the program drew. It's in one of two
 * buffers, so a new text can go in the other while a signal's handler writes the one in use. */
#define TERM_TEXT 1024
static char term_text[2][TERM_TEXT];
static cx_usize term_text_len[2];
static int term_text_in_use;
static int term_text_lock;
static int term_raw_on; /* standard input is in raw mode */
static int term_armed;  /* the handlers that put the terminal back are set */
static int term_ended;
#ifdef _WIN32
static DWORD term_mode;
#else
static struct termios term_mode;
#endif

static void term_cooked(void) {
    if (!__atomic_exchange_n(&term_raw_on, 0, __ATOMIC_ACQ_REL)) return;
#ifdef _WIN32
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), term_mode);
#else
    tcsetattr(0, TCSANOW, &term_mode);
#endif
}

/* Writes the text and puts standard input back, once. `flush` writes what standard output has
 * waiting first, which a signal's handler can't, since it may have stopped a thread that holds the
 * buffer's lock. */
static void term_end(int flush) {
    if (!__atomic_load_n(&term_armed, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&term_ended, 1, __ATOMIC_ACQ_REL)) return;
    int which = __atomic_load_n(&term_text_in_use, __ATOMIC_ACQUIRE);
    cx_usize n = term_text_len[which];
    if (flush) {
        int l = out_lock();
        flush_locked();
        if (n) {
            fwrite(term_text[which], 1, n, stdout);
            fflush(stdout);
        }
        out_unlock(l);
    } else if (n) {
#ifdef _WIN32
        DWORD done;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), term_text[which], (DWORD)n, &done, NULL);
#else
        ssize_t r = write(1, term_text[which], n);
        (void)r;
#endif
    }
    term_cooked();
}

#ifdef _WIN32
/* Runs on a thread of its own, while the others carry on, so it doesn't touch the output buffer.
 * Returning FALSE lets the next handler, in the end the system's, end the program. */
static BOOL WINAPI term_control(DWORD event) {
    (void)event;
    term_end(0);
    return FALSE;
}
#else
/* The handler is reset to the system's as it's entered, and the signal is blocked until it
 * returns, so raising it again ends the program as it would have. */
static void term_signal(int sig) {
    term_end(0);
    raise(sig);
}
#endif

static void term_arm(void) {
    if (__atomic_exchange_n(&term_armed, 1, __ATOMIC_ACQ_REL)) return;
#ifdef _WIN32
    SetConsoleCtrlHandler(term_control, TRUE);
#else
    /* Only where the program, or what started it, hasn't set its own. */
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        struct sigaction was;
        if (sigaction(signals[i], NULL, &was) != 0 || (was.sa_flags & SA_SIGINFO) || was.sa_handler != SIG_DFL) continue;
        struct sigaction a;
        memset(&a, 0, sizeof a);
        a.sa_handler = term_signal;
        a.sa_flags = SA_RESETHAND;
        sigemptyset(&a.sa_mask);
        sigaction(signals[i], &a, NULL);
    }
#endif
}

void cx_term_at_exit(const cx_u8* p, cx_usize n) {
    if (n > TERM_TEXT) n = TERM_TEXT;
    while (__atomic_exchange_n(&term_text_lock, 1, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    int next = 1 - __atomic_load_n(&term_text_in_use, __ATOMIC_ACQUIRE);
    if (n) memcpy(term_text[next], p, n);
    term_text_len[next] = n;
    __atomic_store_n(&term_text_in_use, next, __ATOMIC_RELEASE);
    __atomic_store_n(&term_text_lock, 0, __ATOMIC_RELEASE);
    term_arm();
}

cx_u32 cx_term_raw(void) {
    if (__atomic_load_n(&term_raw_on, __ATOMIC_ACQUIRE)) return 1;
#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return 0;
    term_mode = mode;
    /* Keys come as the VT sequences a terminal sends, such as ESC [ A for the up arrow. */
    DWORD off = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    if (!SetConsoleMode(h, (mode & ~off) | ENABLE_VIRTUAL_TERMINAL_INPUT)) return 0;
#else
    struct termios t;
    if (!isatty(0) || tcgetattr(0, &t) != 0) return 0;
    term_mode = t;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | IXON | PARMRK);
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) return 0;
#endif
    term_arm();
    __atomic_store_n(&term_raw_on, 1, __ATOMIC_RELEASE);
    return 1;
}

void cx_term_cooked(void) { term_cooked(); }

/* The terminal's columns in the high 32 bits and rows in the low, or 0 if standard output isn't
 * one. On Windows it's the console's window, not its buffer, which can be taller. */
cx_u64 cx_term_size(void) {
    cx_u64 columns, rows;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i)) return 0;
    columns = (cx_u64)(i.srWindow.Right - i.srWindow.Left + 1);
    rows = (cx_u64)(i.srWindow.Bottom - i.srWindow.Top + 1);
#else
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) != 0) return 0;
    columns = w.ws_col;
    rows = w.ws_row;
#endif
    if (!columns || !rows) return 0;
    return columns << 32 | rows;
}

/* The code point `c` as UTF-8 at `p`, which has room for 4 bytes. Returns how many it took. */
static cx_usize utf8_put(cx_u8* p, cx_u32 c) {
    if (c < 0x80) {
        p[0] = (cx_u8)c;
        return 1;
    }
    if (c < 0x800) {
        p[0] = (cx_u8)(0xC0 | (c >> 6));
        p[1] = (cx_u8)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        p[0] = (cx_u8)(0xE0 | (c >> 12));
        p[1] = (cx_u8)(0x80 | ((c >> 6) & 0x3F));
        p[2] = (cx_u8)(0x80 | (c & 0x3F));
        return 3;
    }
    p[0] = (cx_u8)(0xF0 | (c >> 18));
    p[1] = (cx_u8)(0x80 | ((c >> 12) & 0x3F));
    p[2] = (cx_u8)(0x80 | ((c >> 6) & 0x3F));
    p[3] = (cx_u8)(0x80 | (c & 0x3F));
    return 4;
}

/* What's typed, as UTF-8. A Windows console's input is UTF-16, read with `ReadConsoleW`, since
 * reading it as bytes loses what isn't ASCII; a surrogate whose pair hasn't come waits for the
 * next read. Outside raw mode, a read that starts with Ctrl+Z is the end of the input, as
 * `ReadFile` makes it, which is what Java's `System.in` and C's `read` see. Elsewhere, and from a
 * pipe or a file, it's `cx_in_read`. `n` is at least 8. */
cx_usize cx_term_read(cx_u8* p, cx_usize n) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (par_depth || h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return cx_in_read(p, n);
    if (out_len) flush_out();
    static wchar_t high;
    wchar_t units[256];
    /* A unit gives at most 6 bytes: U+FFFD for a waiting surrogate it doesn't pair, and itself. */
    DWORD want = (DWORD)(n / 6 < 256 ? n / 6 : 256);
    for (;;) {
        DWORD got = 0;
        if (!ReadConsoleW(h, units, want, &got, NULL)) {
            cx_panic_begin("<stdin>");
            panic_text("can't read standard input");
            cx_panic_end();
        }
        if (got > 0 && units[0] == 0x1A && (mode & ENABLE_PROCESSED_INPUT)) return 0;
        cx_usize out = 0;
        for (DWORD i = 0; i < got; i++) {
            cx_u32 c = units[i];
            if (high) {
                if (c >= 0xDC00 && c <= 0xDFFF) {
                    c = 0x10000 + (((cx_u32)high - 0xD800) << 10) + (c - 0xDC00);
                    high = 0;
                    out += utf8_put(p + out, c);
                    continue;
                }
                high = 0;
                out += utf8_put(p + out, 0xFFFD);
            }
            if (c >= 0xD800 && c <= 0xDBFF) {
                high = (wchar_t)c;
                continue;
            }
            if (c >= 0xDC00 && c <= 0xDFFF) c = 0xFFFD;
            out += utf8_put(p + out, c);
        }
        if (out) return out;
    }
#else
    return cx_in_read(p, n);
#endif
}

/* --- std.process: this process ------------------------------------------------------------- */

/* The memory of this process that's in RAM: its working set on Windows, its resident set on Linux. */
cx_u64 cx_process_memory(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c) ? (cx_u64)c.WorkingSetSize : 0;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size, resident;
    int ok = fscanf(f, "%llu %llu", &size, &resident) == 2;
    fclose(f);
    long page = sysconf(_SC_PAGESIZE);
    return ok && page > 0 ? (cx_u64)resident * (cx_u64)page : 0;
#endif
}

#ifndef _WIN32
/* A cgroup's memory limit, or 0 if it has none: `max`, or no such file. */
static cx_u64 cgroup_limit(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v;
    int ok = fscanf(f, "%llu", &v) == 1;
    fclose(f);
    return ok ? (cx_u64)v : 0;
}

static cx_u64 lower(cx_u64 a, cx_u64 b) { return a && (!b || a < b) ? a : b; }
#endif

/* The memory this process may use: the machine's RAM, or less where a limit says, a job object's
 * on Windows, or the cgroup's on Linux, as a container has, the lowest of its own and its
 * parents'. 0 where it can't say. */
cx_u64 cx_process_memory_limit(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    cx_u64 limit = GlobalMemoryStatusEx(&m) ? (cx_u64)m.ullTotalPhys : 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job;
    if (QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &job, sizeof job, NULL)) {
        if ((job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) && job.ProcessMemoryLimit < limit) limit = job.ProcessMemoryLimit;
        if ((job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) && job.JobMemoryLimit < limit) limit = job.JobMemoryLimit;
    }
    return limit;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    cx_u64 limit = pages > 0 && page > 0 ? (cx_u64)pages * (cx_u64)page : 0;
    /* cgroup v2: the process's group is the line "0::/path" of /proc/self/cgroup. */
    char group[512] = "";
    FILE* f = fopen("/proc/self/cgroup", "r");
    if (f) {
        char line[600];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "0::", 3) == 0) {
                size_t len = strcspn(line + 3, "\n");
                if (len < sizeof group) {
                    memcpy(group, line + 3, len);
                    group[len] = 0;
                }
                break;
            }
        }
        fclose(f);
    }
    char path[600];
    for (;;) {
        snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.max", strcmp(group, "/") == 0 ? "" : group);
        limit = lower(cgroup_limit(path), limit);
        char* slash = strrchr(group, '/');
        if (!slash || group[0] == 0 || strcmp(group, "/") == 0) break;
        if (slash == group) strcpy(group, "/");
        else *slash = 0;
    }
    /* cgroup v1, which says "no limit" with a very large number. */
    return lower(cgroup_limit("/sys/fs/cgroup/memory/memory.limit_in_bytes"), limit);
#endif
}

/* The CPU time every thread of this process has used, in nanoseconds: in the kernel and out. */
cx_u64 cx_process_cpu_ns(void) {
#ifdef _WIN32
    FILETIME made, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &made, &ended, &kernel, &user)) return 0;
    cx_u64 k = (cx_u64)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime;
    cx_u64 u = (cx_u64)user.dwHighDateTime << 32 | user.dwLowDateTime;
    return (k + u) * 100;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return 0;
    return (cx_u64)t.tv_sec * 1000000000u + (cx_u64)t.tv_nsec;
#endif
}

/* The processors this process may run on, as `parallel for` counts them. */
cx_u32 cx_process_processors(void) {
#ifdef _WIN32
    long n = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    cpu_set_t set;
    long n = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? (cx_u32)n : 1;
}

/* Ends the program now with `code`, as returning from `main` does but for the leak check: the
 * terminal is put back, then what was printed is written. Inside a `parallel for`, which
 * iteration got here first would depend on timing, so that's a panic. */
void cx_process_exit(cx_u8 code) {
    if (par_depth) {
        cx_panic_begin("<exit>");
        panic_text("can't exit inside a `parallel for`");
        cx_panic_end();
    }
    take_exit_as(1);
    flush_out();
    fflush(stderr);
    _Exit(code);
}

/* --- std.thread: sleeping ------------------------------------------------------------------ */

/* Waits `ns` nanoseconds, or a little more: Windows rounds up to its timer's milliseconds. */
void cx_thread_sleep(cx_u64 ns) {
#ifdef _WIN32
    cx_u64 ms = ns / 1000000 + (ns % 1000000 != 0);
    while (ms) {
        DWORD now = ms > 0xFFFFFFF0u ? 0xFFFFFFF0u : (DWORD)ms;
        Sleep(now);
        ms -= now;
    }
#else
    struct timespec left = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    while (nanosleep(&left, &left) != 0 && errno == EINTR) {
    }
#endif
}

void cx_print_u64(cx_u64 v) {
    char buf[24];
    int n = cx_u64_text(v, buf + sizeof buf);
    write_bytes(buf + sizeof buf - n, (size_t)n);
}

void cx_print_i64(cx_i64 v) {
    char buf[24];
    int n = cx_u64_text(v < 0 ? (cx_u64)0 - (cx_u64)v : (cx_u64)v, buf + sizeof buf);
    if (v < 0) buf[sizeof buf - ++n] = '-';
    write_bytes(buf + sizeof buf - n, (size_t)n);
}

void cx_print_bool(cx_bool v) {
    if (v) write_bytes("true", 4);
    else write_bytes("false", 5);
}

void cx_print_rune(cx_rune c) {
    cx_u8 b[4];
    write_bytes(b, utf8_put(b, c));
}

/* A lone UTF-16 surrogate can't be written as UTF-8, so it prints as U+FFFD. */
void cx_print_char(cx_char c) { cx_print_rune(c >= 0xD800 && c <= 0xDFFF ? 0xFFFD : c); }

void cx_print_newline(void) { write_bytes("\n", 1); }

/* Floats are printed by calx_float.c, linked only into programs that print one. */

/* --- growing lists --------------------------------------------------------------------------- */

/* Grows a buffer of `*cap` elements to hold len + extra, keeping its first len elements, or with
 * `whole` all of it. */
static void* grow_keeping(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size, int whole) {
    /* No buffer can be bigger than half the address space. */
    cx_usize max = ((cx_usize)-1 / 2) / (size ? size : 1);
    if (len > max || extra > max - len) cx_panic_capacity();
    cx_usize need = len + extra, old = *cap;
    if (need <= old) return ptr;
    cx_usize n = old <= max / 2 ? old * 2 : max;
    if (n < need) n = need;
    if (n < 4 && max >= 4) n = 4;
    *cap = n;
    return cx_realloc_keep(ptr, old * size, n * size, (whole ? old : len) * size);
}

/* A list's elements past its length aren't kept: a list that reserved room it didn't use, and
 * then reserves more, copies what it holds, not the room. */
__attribute__((noinline)) void* cx_grow_to(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    return grow_keeping(ptr, cap, len, extra, size, 0);
}

__attribute__((noinline)) void* cx_deque_grow_to(void* ptr, cx_usize* head, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    cx_usize old = *cap;
    /* A deque's elements can be anywhere in its ring, so all of it is kept. */
    ptr = grow_keeping(ptr, cap, len, extra, size, 1);
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

__attribute__((noinline)) cx_u8* cx_string_grow_to(cx_u8* ptr, cx_usize* cap, cx_usize len, cx_usize extra) {
    if (extra > (cx_usize)-1 / 2 - len) cx_panic_capacity();
    cx_usize need = len + extra, old = *cap;
    if (old == 0) {
        /* Not owned yet: copy the literal's bytes into a buffer of its own. */
        if (need == 0) return ptr;
        cx_usize n = need < 16 ? 16 : need;
        cx_u8* p = (cx_u8*)cx_alloc(n);
        if (len) memcpy(p, ptr, len);
        *cap = n;
        return p;
    }
    if (need <= old) return ptr;
    cx_usize n = old * 2;
    if (n < need) n = need;
    *cap = n;
    return (cx_u8*)cx_realloc_keep(ptr, old, n, len);
}

void cx_string_reserve(cx_String* s, cx_usize extra) {
    cx_usize cap = s->cap;
    s->ptr = cx_string_grow_to(s->ptr, &cap, s->len, extra);
    s->cap = cap;
}

__attribute__((noinline)) cx_u8* cx_string_push_rune_to(cx_u8* ptr, cx_usize* cap, cx_usize* len, cx_rune c) {
    cx_u8 b[4];
    cx_usize n = utf8_put(b, c), at = *len;
    if (at + n > *cap) ptr = cx_string_grow_to(ptr, cap, at, n);
    memcpy(ptr + at, b, n);
    *len = at + n;
    return ptr;
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

/* `s.interned()` (CLX-202): a string that shares one copy of its text with every other interned
 * string of that text, as a literal's string shares the literal. Its capacity is 0, so dropping it
 * frees nothing, a copy of it shares it, and a push copies the bytes first. The copies are never
 * freed, so only names are kept: up to INTERN_LONGEST bytes, INTERN_MOST of them and INTERN_BYTES
 * in all. Past those, it's an ordinary copy.
 *
 * Each name is kept in a chunk after a byte with its length, from malloc, since the chunks and the
 * table outlive the program's values and a debug build counts what's left of those. A table
 * behind a lock finds them, and
 * each thread keeps the last name it was given at each of INTERN_CACHE places, a fixed array so a
 * thread that ends leaves nothing behind. A parse that interns each tag takes the lock only for a
 * name it hasn't met. */
#define INTERN_LONGEST 64
#define INTERN_MOST 65536u
#define INTERN_BYTES ((cx_usize)1 << 20)
#define INTERN_CHUNK ((cx_usize)16384)
#define INTERN_CACHE 128u

static const cx_u8** intern_table; /* a power of two, at most half full, of the names' bytes */
static cx_usize intern_mask, intern_count, intern_bytes;
static cx_u8* intern_room;
static cx_usize intern_left;
static _Thread_local const cx_u8* intern_cache[INTERN_CACHE];
#ifdef _WIN32
static SRWLOCK intern_mutex = SRWLOCK_INIT;
#else
static pthread_mutex_t intern_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static int intern_same(const cx_u8* p, cx_str s) { return p[-1] == s.len && memcmp(p, s.ptr, s.len) == 0; }

/* The kept copy of `s`, hashed `h`, adding it if there's room, or 0. Called with the lock held. */
static const cx_u8* intern_find(cx_str s, cx_u64 h) {
    if (intern_table) {
        for (cx_usize i = (cx_usize)h & intern_mask;; i = (i + 1) & intern_mask) {
            const cx_u8* p = intern_table[i];
            if (!p) break;
            if (intern_same(p, s)) return p;
        }
    }
    if (intern_count >= INTERN_MOST || intern_bytes + s.len + 1 > INTERN_BYTES) return 0;
    if (!intern_table || (intern_count + 1) * 2 > intern_mask + 1) {
        cx_usize size = intern_table ? (intern_mask + 1) * 2 : 256;
        const cx_u8** t = (const cx_u8**)calloc(size, sizeof(*t));
        if (!t) return 0;
        for (cx_usize j = 0; intern_table && j <= intern_mask; j++) {
            const cx_u8* p = intern_table[j];
            if (!p) continue;
            cx_usize i = (cx_usize)cx_hash_bytes(p, p[-1]) & (size - 1);
            while (t[i]) i = (i + 1) & (size - 1);
            t[i] = p;
        }
        free((void*)intern_table);
        intern_table = t;
        intern_mask = size - 1;
    }
    if (intern_left < s.len + 1) {
        cx_u8* room = (cx_u8*)malloc(INTERN_CHUNK);
        if (!room) return 0;
        intern_room = room;
        intern_left = INTERN_CHUNK;
    }
    cx_u8* p = intern_room + 1;
    p[-1] = (cx_u8)s.len;
    memcpy(p, s.ptr, s.len);
    intern_room += s.len + 1;
    intern_left -= s.len + 1;
    intern_bytes += s.len + 1;
    cx_usize i = (cx_usize)h & intern_mask;
    while (intern_table[i]) i = (i + 1) & intern_mask;
    intern_table[i] = p;
    intern_count++;
    return p;
}

cx_String cx_string_interned(cx_str s) {
    if (s.len == 0 || s.len > INTERN_LONGEST) return cx_string_from(s);
    cx_u64 h = cx_hash_bytes(s.ptr, s.len);
    const cx_u8** slot = &intern_cache[(h >> 32) & (INTERN_CACHE - 1)];
    const cx_u8* p = *slot;
    if (!p || !intern_same(p, s)) {
#ifdef _WIN32
        AcquireSRWLockExclusive(&intern_mutex);
        p = intern_find(s, h);
        ReleaseSRWLockExclusive(&intern_mutex);
#else
        pthread_mutex_lock(&intern_mutex);
        p = intern_find(s, h);
        pthread_mutex_unlock(&intern_mutex);
#endif
        if (!p) return cx_string_from(s);
        *slot = p;
    }
    cx_String r = {(cx_u8*)p, s.len, 0};
    return r;
}

/* How many of the `n` bytes at `p` come before the first that's `limit` or more: 16 at a time
 * where the processor has SSE2, which every x86-64 one does. std.unicode skips text that can't
 * change with it. */
cx_usize cx_bytes_below(const cx_u8* p, cx_usize n, cx_u8 limit) {
    cx_usize i = 0;
    if (limit == 0) return 0;
#if defined(__SSE2__)
    __m128i top = _mm_set1_epi8((char)(limit - 1));
    while (n - i >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i));
        int below = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_max_epu8(v, top), top));
        if (below != 0xFFFF) return i + (cx_usize)__builtin_ctz(~below & 0xFFFF);
        i += 16;
    }
#endif
    while (i < n && p[i] < limit) i++;
    return i;
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

/* The digits in base `radix`, from 2 to 36, after an optional sign, up to `limit`: `0` to `9`,
 * then letters in either case, as Java's `Character.digit` takes ASCII. */
static cx_bool parse_radix_digits(const cx_u8* p, cx_usize n, cx_u32 radix, cx_u64 limit, cx_u64* out) {
    if (n == 0) return 0;
    cx_u64 v = 0;
    for (cx_usize i = 0; i < n; i++) {
        unsigned b = p[i], d = b - '0';
        if (d > 9) d = (b | 0x20) - 'a' < 26 ? (b | 0x20) - 'a' + 10 : 99;
        if (d >= radix) return 0;
        if (d > limit || v > (limit - d) / radix) return 0;
        v = v * radix + d;
    }
    *out = v;
    return 1;
}

cx_bool cx_parse_i64_radix(cx_str s, cx_u32 radix, cx_i64 lo, cx_i64 hi, cx_i64* out) {
    *out = 0;
    if (s.len == 0) return 0;
    int neg = s.ptr[0] == '-';
    cx_usize skip = neg || s.ptr[0] == '+';
    cx_u64 v;
    if (!parse_radix_digits(s.ptr + skip, s.len - skip, radix, neg ? (cx_u64)0 - (cx_u64)lo : (cx_u64)hi, &v)) return 0;
    *out = neg ? (cx_i64)((cx_u64)0 - v) : (cx_i64)v;
    return 1;
}

cx_bool cx_parse_u64_radix(cx_str s, cx_u32 radix, cx_u64 hi, cx_u64* out) {
    *out = 0;
    if (s.len == 0 || s.ptr[0] == '-') return 0;
    cx_usize skip = s.ptr[0] == '+';
    return parse_radix_digits(s.ptr + skip, s.len - skip, radix, hi, out);
}

void cx_panic_radix(cx_u32 radix, const char* at) {
    char text[64];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "a radix is from 2 to 36, not %u", (unsigned)radix);
    panic_text(text);
    cx_panic_end();
}

cx_bool cx_str_contains(cx_str hay, cx_str needle) { return cx_str_find(hay, needle) != CX_NOT_FOUND; }

/* How often a byte turns up in text, roughly, from 0 for the rarest: spaces and lowercase letters
 * most, in English's order, then line ends and common punctuation, digits, capitals, bytes of
 * UTF-8, other punctuation, and control bytes least. */
static int byte_commonness(cx_u8 b) {
    /* a to z, 250 for `e` down by 3 a place in "etaoinsrhldcumfpgwybvkxjqz". */
    static const cx_u8 letters[26] = {244, 193, 217, 220, 250, 208, 202, 226, 238, 181, 187, 223, 211,
                                      235, 241, 205, 178, 229, 232, 247, 214, 190, 199, 184, 196, 175};
    if (b == ' ') return 255;
    if (b >= 'a' && b <= 'z') return letters[b - 'a'];
    if (b == '\n' || b == '.' || b == ',') return 170;
    if (b >= '0' && b <= '9') return 150 - (b - '0');
    if (b >= 'A' && b <= 'Z') return 120;
    if (b == '\t' || b == '\r') return 110;
    if (b >= 0x80) return b >= 0xC0 ? 90 : 100;
    if (b < 0x20 || b == 0x7F) return 5;
    return 80;
}

/* Where `needle` first is in the `len` bytes at `hay`. See `cx_bytes_find_rare`. */
cx_usize cx_bytes_find(const cx_u8* hay, cx_usize len, const cx_u8* needle, cx_usize n) {
    return cx_bytes_find_rare(hay, len, needle, n, cx_bytes_rare(needle, n));
}

/* Which two of the `n` bytes at `needle` are likely the rarest in text, for `cx_bytes_find_rare`:
 * the rarest's place, and the next's shifted up 16 bits, both among the first 65536. */
cx_usize cx_bytes_rare(const cx_u8* needle, cx_usize n) {
    cx_usize m = n < 65536 ? n : 65536;
    cx_usize k1 = 0, k2 = 0;
    int b1 = 1000, b2 = 1000;
    for (cx_usize i = 0; i < m; i++) {
        int c = byte_commonness(needle[i]);
        if (c < b1) {
            k2 = k1;
            b2 = b1;
            k1 = i;
            b1 = c;
        } else if (c < b2) {
            k2 = i;
            b2 = c;
        }
    }
    if (m > 1 && k2 == k1) k2 = k1 == 0 ? 1 : 0;
    return k1 | (k2 << 16);
}

/* Whether the `n` bytes at `s` are the needle's. A short needle is compared a byte at a time,
 * which beats calling `memcmp` for the few bytes there are. */
static inline int bytes_same(const cx_u8* s, const cx_u8* needle, cx_usize n) {
    if (n <= 32) {
        cx_usize j = 0;
        while (j < n && s[j] == needle[j]) j++;
        return j == n;
    }
    return memcmp(s, needle, n) == 0;
}

/* `cx_bytes_find`, given what `cx_bytes_rare` says of the needle, for a search that looks for the
 * same needle again and again. With SIMD, each step tests 32 places the needle could start, or
 * 16, for both of its rarest bytes at once, and only where both are there is the rest compared:
 * that's rarely, even when each byte alone is common. Without it, `memchr` finds the rarest. */
#if defined(__AVX2__)
typedef char find_v __attribute__((vector_size(32), aligned(1)));
#define FIND_STEP 32
#define FIND_MASK(v) ((cx_u32)__builtin_ia32_pmovmskb256(v))
#elif defined(__SSE2__)
typedef char find_v __attribute__((vector_size(16), aligned(1)));
#define FIND_STEP 16
#define FIND_MASK(v) ((cx_u32)__builtin_ia32_pmovmskb128(v))
#endif
cx_usize cx_bytes_find_rare(const cx_u8* hay, cx_usize len, const cx_u8* needle, cx_usize n, cx_usize k) {
    if (n == 0) return 0;
    if (n > len) return CX_NOT_FOUND;
    if (n == 1) {
        const cx_u8* q = (const cx_u8*)memchr(hay, needle[0], (size_t)len);
        return q ? (cx_usize)(q - hay) : CX_NOT_FOUND;
    }
    cx_usize k1 = k & 0xFFFF, k2 = (k >> 16) & 0xFFFF;
    cx_u8 c1 = needle[k1], c2 = needle[k2];
    cx_usize last = len - n;  // the last place the needle can start
    cx_usize p = 0;
#if defined(FIND_STEP)
    /* Each step's loads end at p + k + FIND_STEP - 1, which is inside the text while the starts it
     * tests, up to p + FIND_STEP - 1, are no later than `last`. */
    while (p <= last && last - p >= FIND_STEP - 1) {
        find_v a = *(const find_v*)(hay + p + k1);
        find_v b = *(const find_v*)(hay + p + k2);
        cx_u32 m = FIND_MASK((a == (char)c1) & (b == (char)c2));
        while (m) {
            cx_usize j = p + (cx_usize)__builtin_ctz(m);
            if (bytes_same(hay + j, needle, n)) return j;
            m &= m - 1;
        }
        p += FIND_STEP;
    }
    for (; p <= last; p++) {
        if (hay[p + k1] == c1 && hay[p + k2] == c2 && bytes_same(hay + p, needle, n)) return p;
    }
    return CX_NOT_FOUND;
#else
    const cx_u8* q = hay + k1;
    const cx_u8* stop = hay + last + k1 + 1;  // past the last place the rarest byte can be
    while (q < stop) {
        q = (const cx_u8*)memchr(q, c1, (size_t)(stop - q));
        if (!q) return CX_NOT_FOUND;
        const cx_u8* s = q - k1;
        if (s[k2] == c2 && bytes_same(s, needle, n)) return (cx_usize)(s - hay);
        q++;
    }
    return CX_NOT_FOUND;
#endif
}
cx_usize cx_str_find(cx_str hay, cx_str needle) { return cx_bytes_find(hay.ptr, hay.len, needle.ptr, needle.len); }

/* For std.regex: the first byte from `from` that a match can start with, or `len` if there's none.
 * `set` has a nonzero entry for each of them, `count` of them, and with three or fewer, `bytes`
 * holds them, one a byte. One is found by `memchr`; two or three by testing many bytes at once
 * for any of them, 32 or 16 with SIMD and 8 without, then looking at those one by one only when
 * one is there; more by the table, a byte at a time. */
#define RX_ONES 0x0101010101010101ull
#define RX_HIGHS 0x8080808080808080ull
#define RX_HAS_ZERO(v) (((v) - RX_ONES) & ~(v) & RX_HIGHS)
cx_usize cx_rx_skip(const cx_u8* hay, cx_usize len, cx_usize from, const cx_u8* set, cx_u32 count, cx_u32 bytes) {
    const cx_u8* p = hay + from;
    const cx_u8* end = hay + len;
    if (count == 1) {
        const cx_u8* q = (const cx_u8*)memchr(p, (int)(bytes & 0xFF), (size_t)(end - p));
        return q ? (cx_usize)(q - hay) : len;
    }
    if (count == 2 || count == 3) {
#if defined(FIND_STEP)
        char ca = (char)(bytes & 0xFF);
        char cb = (char)((bytes >> 8) & 0xFF);
        char cc = (char)(count == 3 ? (bytes >> 16) & 0xFF : (bytes >> 8) & 0xFF);
        while (end - p >= FIND_STEP) {
            find_v w = *(const find_v*)p;
            if (FIND_MASK((w == ca) | (w == cb) | (w == cc))) break;
            p += FIND_STEP;
        }
#endif
        cx_u64 a = RX_ONES * (bytes & 0xFF);
        cx_u64 b = RX_ONES * ((bytes >> 8) & 0xFF);
        cx_u64 c = count == 3 ? RX_ONES * ((bytes >> 16) & 0xFF) : b;
        while (end - p >= 8) {
            cx_u64 w;
            memcpy(&w, p, 8);
            if (RX_HAS_ZERO(w ^ a) | RX_HAS_ZERO(w ^ b) | RX_HAS_ZERO(w ^ c)) break;
            p += 8;
        }
    }
    while (p < end && !set[*p]) p++;
    return (cx_usize)(p - hay);
}

/* For std.csv: the first byte from `from` that's `a`, `b` or `c`, or `len`, which ends a field
 * without quotes at the delimiter or a line break (CLX-188). A field is short, so the first step
 * mostly decides it: 16 or 32 bytes compared at once, the place taken from the mask, with no byte
 * loop after it. Only the last few bytes of the text go a byte at a time. */
cx_usize cx_bytes_find3(const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 a, cx_u8 b, cx_u8 c) {
    const cx_u8* p = hay + from;
    const cx_u8* end = hay + len;
#if defined(FIND_STEP)
    while (end - p >= FIND_STEP) {
        find_v w = *(const find_v*)p;
        cx_u32 m = FIND_MASK((w == (char)a) | (w == (char)b) | (w == (char)c));
        if (m) return (cx_usize)(p - hay) + (cx_usize)__builtin_ctz(m);
        p += FIND_STEP;
    }
#endif
    while (p < end && *p != a && *p != b && *p != c) p++;
    return (cx_usize)(p - hay);
}

/* For std.html's `escape`: the same for five bytes, `&`, `<`, `>`, `"` and `'` (CLX-188). */
cx_usize cx_bytes_find5(const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 a, cx_u8 b, cx_u8 c, cx_u8 d, cx_u8 e) {
    const cx_u8* p = hay + from;
    const cx_u8* end = hay + len;
#if defined(FIND_STEP)
    while (end - p >= FIND_STEP) {
        find_v w = *(const find_v*)p;
        cx_u32 m = FIND_MASK((w == (char)a) | (w == (char)b) | (w == (char)c) | (w == (char)d) | (w == (char)e));
        if (m) return (cx_usize)(p - hay) + (cx_usize)__builtin_ctz(m);
        p += FIND_STEP;
    }
#endif
    while (p < end && *p != a && *p != b && *p != c && *p != d && *p != e) p++;
    return (cx_usize)(p - hay);
}

/* For std.textwrap: the first byte from `from` that could start a line break as Python's
 * `splitlines` breaks lines, or `len`: a control below 0x1F, or 0xC2 or 0xE2, which lead U+0085 and
 * U+2028 and U+2029. Each is ASCII or a lead byte, so it's always where a rune starts. */
cx_usize cx_bytes_find_break(const cx_u8* hay, cx_usize len, cx_usize from) {
    const cx_u8* p = hay + from;
    const cx_u8* end = hay + len;
#if defined(FIND_STEP)
    while (end - p >= FIND_STEP) {
        find_v w = *(const find_v*)p;
        cx_u32 m = FIND_MASK(((w > (char)-1) & (w < (char)0x1F)) | (w == (char)0xC2) | (w == (char)0xE2));
        if (m) return (cx_usize)(p - hay) + (cx_usize)__builtin_ctz(m);
        p += FIND_STEP;
    }
#endif
    while (p < end && *p >= 0x1F && *p != 0xC2 && *p != 0xE2) p++;
    return (cx_usize)(p - hay);
}

/* For std.unicode's quick check: the first byte from `from`, a rune's start, that leads a rune
 * other than these, or `len`: a byte below `low`, and with `kinds` 1, 2 and 4 the three-byte runes
 * of CJK's ideographs, U+4E00 to U+9FFF; Hangul's syllables, U+AC00 to U+D7FF; and Latin Extended
 * Additional, U+1E00 to U+1EFF, which Vietnamese is written in. std.unicode passes the ones its
 * tables say are starters certainly in the form. Continuation bytes are passed over one by one,
 * after their lead, so what's returned is always a lead byte, where a rune starts. Each step
 * tests a lead with the byte after it, so it reads one byte past the step. */
cx_usize cx_bytes_skip_starters(const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 low, cx_u8 kinds) {
    const cx_u8* p = hay + from;
    const cx_u8* end = hay + len;
#if defined(FIND_STEP)
    typedef cx_u8 skip_u __attribute__((vector_size(FIND_STEP), aligned(1)));
    cx_u32 all = FIND_STEP == 32 ? 0xFFFFFFFFu : 0xFFFFu;
    while (end - p >= FIND_STEP + 1) {
        skip_u w = *(const skip_u*)p;
        find_v ok = (find_v)(w < low);
        /* A step of only bytes below `low`, as ASCII text is, needs nothing more. */
        if ((FIND_MASK(ok) & all) != all) {
            skip_u x = *(const skip_u*)(p + 1);
            ok |= (find_v)((w & 0xC0) == 0x80);
            if (kinds & 1) ok |= (find_v)(((w >= 0xE5) & (w <= 0xE9)) | ((w == 0xE4) & (x >= 0xB8)));
            if (kinds & 2) ok |= (find_v)(((w >= 0xEB) & (w <= 0xED)) | ((w == 0xEA) & (x >= 0xB0)));
            if (kinds & 4) ok |= (find_v)((w == 0xE1) & (x >= 0xB8) & (x <= 0xBB));
            cx_u32 m = ~FIND_MASK(ok) & all;
            if (m) return (cx_usize)(p - hay) + (cx_usize)__builtin_ctz(m);
        }
        p += FIND_STEP;
    }
#endif
    while (p < end) {
        cx_u8 b = *p;
        cx_u8 x = end - p > 1 ? p[1] : 0;
        if (!(b < low || (b & 0xC0) == 0x80 ||
              ((kinds & 1) && ((b >= 0xE5 && b <= 0xE9) || (b == 0xE4 && x >= 0xB8))) ||
              ((kinds & 2) && ((b >= 0xEB && b <= 0xED) || (b == 0xEA && x >= 0xB0))) ||
              ((kinds & 4) && b == 0xE1 && x >= 0xB8 && x <= 0xBB)))
            break;
        p++;
    }
    return (cx_usize)(p - hay);
}
#undef RX_ONES
#undef RX_HIGHS
#undef RX_HAS_ZERO
#undef FIND_STEP
#undef FIND_MASK

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

void cx_panic_elements(cx_usize lo, cx_usize hi, cx_bool inclusive, cx_usize length, const char* at) {
    char text[128];
    cx_usize end = hi + (inclusive ? 1 : 0);
    const char* dots = inclusive ? "..=" : "..";
    unsigned long long a = (unsigned long long)lo, b = (unsigned long long)hi;
    cx_panic_begin(at);
    if (end > length || end < hi) {
        snprintf(text, sizeof text, "the range %llu%s%llu is out of bounds for length %llu", a, dots, b, (unsigned long long)length);
    } else {
        snprintf(text, sizeof text, "the range %llu%s%llu starts after it ends", a, dots, b);
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

void cx_panic_truncate(cx_usize n, cx_usize length, const char* at) {
    char text[112];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "truncating to %llu bytes cuts a rune apart: byte %llu is inside one, in text of length %llu",
             (unsigned long long)n, (unsigned long long)n, (unsigned long long)length);
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

void cx_print_handle(cx_handle h) {
    write_bytes("Handle(", 7);
    cx_print_u64(h.index);
    write_bytes(", ", 2);
    cx_print_u64(h.gen);
    write_bytes(")", 1);
}

void cx_panic_handle(cx_handle h, const char* at) {
    char text[128];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "the slot map has no value for Handle(%u, %u): it was removed, or the handle is another map's",
             (unsigned)h.index, (unsigned)h.gen);
    panic_text(text);
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

void cx_panic_key_marks(cx_usize marks, cx_usize keys, const char* at) {
    char text[112];
    cx_panic_begin(at);
    snprintf(text, sizeof text, "`remove_marked` needs a mark for each key: %llu marks for %llu keys", (unsigned long long)marks,
             (unsigned long long)keys);
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

/* A table's slots, rounded up to a group so the control bytes after them start on one, then its
 * control bytes and their copy, then what else it keeps. */
static cx_usize table_slot_bytes(cx_usize buckets, cx_usize size) { return (buckets * size + 15) & ~(cx_usize)15; }

cx_u8* cx_table_alloc(cx_usize buckets, cx_usize size, cx_usize extra) {
    if (buckets > ((cx_usize)-1 >> 2) / (size + 1) || extra > ((cx_usize)-1 >> 2)) cx_panic_capacity();
    cx_usize bytes = table_slot_bytes(buckets, size);
    cx_u8* ctrl = (cx_u8*)cx_alloc(bytes + buckets + CX_GROUP + extra) + bytes;
    memset(ctrl, CX_CTRL_EMPTY, buckets + CX_GROUP);
    return ctrl;
}

void cx_table_free(cx_u8* ctrl, cx_usize buckets, cx_usize size, cx_usize extra) {
    cx_usize bytes = table_slot_bytes(buckets, size);
    cx_free(ctrl - bytes, bytes + buckets + CX_GROUP + extra);
}

/* --- parallel for ---------------------------------------------------------------------------
 * `parallel for` runs its body on a pool of threads that the calling thread takes part in. The
 * iterations are split into chunks. Each thread of a loop has a range of them, which it runs in
 * order, and a thread whose range is used up takes half of what's left of another's: all of it,
 * if that thread hasn't come yet (CLX-257). So a thread claims its chunks from a cache line of
 * its own, and a loop of cheap iterations doesn't have every thread wait on one counter, while
 * one of slow or uneven iterations still evens out. The checker makes sure no two iterations
 * touch the same memory (D21), so what they compute can't depend on which thread ran what.
 * What they print is kept per chunk and written in chunk order when the loop ends, and a panic
 * reports the first iteration that panicked, after the output of every iteration before it. So
 * the output is the same with one thread and with many, too.
 *
 * The outermost loop is the top job, which the main thread opens. Workers wait for it on its
 * word, which is odd while it's open: they spin for a while, since loops often come one after
 * another, then sleep on it. A worker joins the top job by writing the job's word into its own
 * line, a cache line no other thread writes, and then checking that the word hasn't changed, and
 * once it has no chunks left to run it adds how many it ran (CLX-258). The job's owner adds those
 * up until every chunk has run, closes the job, and waits for each worker's line to let go of it,
 * so no thread reads a job after that, a sleeping thread is never waited for, and no two threads
 * write the same cache line to join and leave. An inner job counts its helpers in its `active`
 * instead, since few threads come to one.
 *
 * A loop whose iterations each took less than `PAR_ALONE_NS` the last time it ran starts on the
 * thread that reaches it, in batches that double, and hands the rest to the pool once what's
 * left looks like more than `PAR_ALONE_US` of work at the rate so far. So a small loop costs
 * about what a `for` does, and a big one starts on the pool an iteration or so late. Each
 * loop's body is a function of its own, so a table keyed by it holds each loop's time an
 * iteration, measured on the thread that runs the loop. A loop inside one that runs alone,
 * outside a chunk, runs on the same pool, which nothing else is using then.
 *
 * The top job's chunks start out split evenly between its threads. An inner job's start out
 * with its owner, and the threads that help take theirs from there. After a panic, the chunks
 * before the first one that panicked still run, and the rest are skipped.
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
#define PAR_NESTED 64       /* inner jobs open at once, one bit each of a pool's `open` */
#define PAR_SHARED __attribute__((aligned(128))) /* its own pair of cache lines */
#define PAR_ALONE_US 3      /* a loop that looks like less work than this runs on one thread */
#define PAR_ALONE_NS 500    /* a loop whose iterations take less than this starts on one thread */
#define PAR_SITES 1024      /* loops whose time an iteration is kept */

typedef void (*par_body)(void* ctx, cx_u64 begin, cx_u64 end);

/* A thread's range of a loop's chunks, `lo` to `hi`, as `lo << 32 | hi`, with `PAR_JOINED` set
 * once its thread has come. Its thread takes chunks from `lo`, and others take them from `hi`. */
typedef struct {
    PAR_SHARED cx_u64 word;
} par_range;

#define PAR_JOINED ((cx_u64)1 << 63)
#define PAR_LO(w) ((cx_u32)((w) >> 32) & 0x7fffffffu)
#define PAR_HI(w) ((cx_u32)(w))
#define PAR_RANGE(lo, hi) ((cx_u64)(lo) << 32 | (cx_u64)(hi))

/* A chunk's output: segments of one byte naming the stream (1 for standard error), the length,
 * and the bytes. */
typedef struct {
    unsigned char* p;
    size_t len, cap;
    size_t last; /* where the last segment starts, or SIZE_MAX */
} par_out;

struct par_pool;

/* A loop's chunks. Its owner writes the first part before opening it, and nothing changes that
 * while it's open. */
typedef struct par_job {
    par_body body;
    void* ctx;
    cx_u64 base, n, chunk, chunks; /* iterations `base` to `base + n` */
    unsigned char* done; /* per chunk: whether it has run, for a panic */
    par_out** outs;      /* per chunk: what it printed, once it prints */
    cx_u64 cap;          /* the length of both */
    par_range* ranges;   /* per thread of the loop, by `par_slot` */
    cx_u32 slots;        /* its threads */
    cx_u32 ranges_cap;   /* the length of `ranges` */
    struct par_pool* pool; /* the pool it runs on */
    /* An inner job: the chunk its owner was running when it opened it, where its output goes. */
    struct par_job* up;
    cx_u64 up_chunk;
    int up_owner;
    unsigned slot;
    PAR_SHARED cx_u32 word;   /* jobs opened * 2, plus 1 while one is open */
    PAR_SHARED cx_u32 active; /* an inner job: threads in it besides its owner */
    PAR_SHARED cx_u64 panic;  /* the first chunk that panicked */
} par_job;

/* A worker's own cache line: the top job it's in, `word << 32`, plus 1 and the chunks of it the
 * worker ran once it has none left to run, or 0 outside one. Only the worker writes it. */
typedef struct {
    PAR_SHARED cx_u64 in;
} par_line;

/* A pool of threads: the main thread's, which a `parallel for` runs on unless it names another,
 * or one a program made with std.pool's `Pool.new`, which `parallel(pool) for` runs on (CLX-71).
 * The thread that runs a loop on it owns its top job, and its workers help with that and with
 * the inner jobs the top job's chunks open. One thread at a time runs a loop on a pool of the
 * program's; another waits for it. */
typedef struct par_pool {
    par_job top;
    par_job nest[PAR_NESTED];
    PAR_SHARED cx_u64 taken;         /* bit k: a thread owns `nest[k]` */
    PAR_SHARED cx_u64 open;          /* bit k: `nest[k]` is open */
    PAR_SHARED cx_u32 sleepers;      /* workers asleep on the top job's word */
    PAR_SHARED cx_u32 help_word;     /* changes when an inner job opens or the top job's chunks finish */
    PAR_SHARED cx_u32 help_sleepers; /* threads asleep on it */
    PAR_SHARED cx_u32 busy;          /* a program's pool: a thread runs a loop on it */
    PAR_SHARED cx_u32 busy_sleepers; /* threads asleep waiting for it */
    PAR_SHARED cx_u32 alive;         /* a program's pool: workers that haven't stopped */
    cx_u32 stop;                     /* a program's pool: its workers stop */
    par_line** lines;                /* each worker's, by its number, which only the owner reads */
    cx_usize lines_cap;
    cx_usize target;  /* threads to use, its owner included; 0 in the main pool until set or read */
    cx_usize started; /* worker threads started */
} par_pool;

static par_pool par_main = {.top = {.panic = PAR_NONE}};

static _Thread_local par_job* par_cur; /* the job whose chunk this thread is running */
static _Thread_local cx_u64 par_chunk; /* that chunk */
static _Thread_local int par_owner;    /* this thread owns `par_cur` */
static _Thread_local int par_worker;   /* this is a worker thread */
static _Thread_local int par_ending;   /* the inner jobs this thread is ending a panic in */
/* This thread's range in its pool's jobs: 0 for the top job's owner, a worker's number + 1. */
static _Thread_local cx_u32 par_slot;
static _Thread_local cx_u32 par_victim; /* the range it last took chunks from */
static _Thread_local par_line* par_mine; /* a worker's own line */
/* The pool of the loop this thread runs alone, outside a chunk. */
static _Thread_local struct par_pool* par_home;

static void par_pause(void) { CX_SPIN_PAUSE(); }

/* A clock for timing loops, in what it counts: `par_tick_hz` a second. */
#ifdef _WIN32
static cx_u64 par_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (cx_u64)now.QuadPart;
}
#define par_tick_hz counter_hz
#else
static cx_u64 par_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (cx_u64)ts.tv_sec * 1000000000u + (cx_u64)ts.tv_nsec;
}
#define par_tick_hz 1000000000u
#endif

/* A loop's time an iteration, in ticks << 20, by its body: 0 if it hasn't run, and at least 1 once
 * it has. Threads read and write it without a lock, since a wrong guess only costs time. */
typedef struct {
    par_body body;
    cx_u64 cost;
} par_site;

static par_site par_sites[PAR_SITES];

static par_site* par_site_of(par_body body) {
    cx_u64 h = (cx_u64)(uintptr_t)body * 0x9E3779B97F4A7C15u;
    return &par_sites[h >> 54];
}

static cx_u64 par_cost(par_site* site, par_body body) {
    if (__atomic_load_n(&site->body, __ATOMIC_RELAXED) != body) return 0;
    return __atomic_load_n(&site->cost, __ATOMIC_RELAXED);
}

/* Keeps `iters` iterations in `ticks` as the loop's time an iteration, unless it's within a factor
 * of 2 of what's kept, so threads running the same loop don't all write it. */
static void par_learn(par_site* site, par_body body, cx_u64 ticks, cx_u64 iters) {
    if (!iters) return;
    cx_u64 cost = ticks >= ((cx_u64)1 << 43) ? ~(cx_u64)0 >> 1 : (ticks << 20) / iters;
    if (cost == 0) cost = 1;
    cx_u64 was = par_cost(site, body);
    if (was && cost <= was * 2 && cost * 2 >= was) return;
    __atomic_store_n(&site->body, body, __ATOMIC_RELAXED);
    __atomic_store_n(&site->cost, cost, __ATOMIC_RELAXED);
}

/* Whether a loop whose iterations take `cost` starts on one thread. */
static int par_cheap(cx_u64 cost) {
    return cost && cost < ((cx_u64)par_tick_hz * PAR_ALONE_NS / 1000000000u << 20);
}

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

/* Wakes the threads helping `p`'s inner jobs: one has opened, or the top job's chunks have
 * finished. */
static void par_nudge(par_pool* p) {
    __atomic_fetch_add(&p->help_word, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->help_sleepers, __ATOMIC_SEQ_CST)) par_wake(&p->help_word);
}

/* Moves part of another thread's range of `j` into `mine`, which is empty: half of what's left,
 * rounded up, or all of it if that thread hasn't come. Returns 0 if every other range is empty.
 * The ranges only hold chunk numbers, so they need no ordering of their own. */
static int par_steal(par_job* j, par_range* mine) {
    cx_u32 slots = j->slots;
    cx_u32 v = par_victim < slots ? par_victim : 0;
    for (cx_u32 k = 0; k < slots; k++, v = v + 1 == slots ? 0 : v + 1) {
        par_range* r = &j->ranges[v];
        if (r == mine) continue;
        cx_u64 w = __atomic_load_n(&r->word, __ATOMIC_RELAXED);
        for (;;) {
            cx_u32 lo = PAR_LO(w), hi = PAR_HI(w);
            if (lo >= hi) break;
            cx_u32 mid = w & PAR_JOINED ? hi - (hi - lo + 1) / 2 : lo;
            if (__atomic_compare_exchange_n(&r->word, &w, (w & PAR_JOINED) | PAR_RANGE(lo, mid), 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                __atomic_store_n(&mine->word, PAR_JOINED | PAR_RANGE(mid, hi), __ATOMIC_RELAXED);
                par_victim = v;
                return 1;
            }
        }
    }
    return 0;
}

/* What a loop's owner measures: the ticks it took over the chunks of its own range, and their
 * iterations, until it ran out of them. */
typedef struct {
    cx_u64 ticks, iters;
} par_meter;

/* Runs chunks of `j` from this thread's range, and from others' once that's used up, until none
 * are left, as its owner or as a helper. After a panic it skips the chunks from the first one
 * that panicked on. Its owner passes `m`. Never inlined, so a stack trace tells a chunk's body
 * from a loop that runs where it is. */
static __attribute__((noinline)) cx_u64 par_claim(par_job* j, int owner, par_meter* m) {
    par_job* was = par_cur;
    cx_u64 was_chunk = par_chunk;
    int was_owner = par_owner;
    par_cur = j;
    par_owner = owner;
    par_buffered = 1;
    par_range* mine = &j->ranges[par_slot];
    cx_u64 ran = 0, start = m ? par_ticks() : 0, iters = 0;
    for (;;) {
        cx_u64 w = __atomic_load_n(&mine->word, __ATOMIC_RELAXED);
        cx_u32 lo = PAR_LO(w);
        if (lo >= PAR_HI(w)) {
            if (m) {
                m->ticks = par_ticks() - start;
                m->iters = iters;
                m = 0;
            }
            if (par_steal(j, mine)) continue;
            break;
        }
        if (!__atomic_compare_exchange_n(&mine->word, &w, PAR_JOINED | (w + ((cx_u64)1 << 32)), 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        cx_u64 c = lo;
        if (c >= __atomic_load_n(&j->panic, __ATOMIC_RELAXED)) continue;
        par_chunk = c;
        cx_u64 begin = c * j->chunk;
        cx_u64 end = j->n - begin < j->chunk ? j->n : begin + j->chunk;
        j->body(j->ctx, j->base + begin, j->base + end);
        __atomic_store_n(&j->done[c], 1, __ATOMIC_RELEASE);
        ran++;
        iters += end - begin;
    }
    par_cur = was;
    par_chunk = was_chunk;
    par_owner = was_owner;
    par_buffered = was != 0;
    return ran;
}

/* Runs chunks of every open inner job of `p` once. Returns whether there were any. */
static int par_help_once(par_pool* p) {
    cx_u64 open = __atomic_load_n(&p->open, __ATOMIC_ACQUIRE);
    int helped = 0;
    while (open) {
        par_job* j = &p->nest[__builtin_ctzll(open)];
        open &= open - 1;
        cx_u32 w = __atomic_load_n(&j->word, __ATOMIC_SEQ_CST);
        if (!(w & 1)) continue;
        __atomic_fetch_add(&j->active, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&j->word, __ATOMIC_SEQ_CST) == w) {
            par_claim(j, 0, 0);
            helped = 1;
        }
        __atomic_fetch_sub(&j->active, 1, __ATOMIC_RELEASE);
    }
    return helped;
}

/* On a worker with no chunks of `p`'s top job left to run, its word `w`: helps inner jobs until
 * the owner closes the job, once every chunk has run, sleeping when there's been nothing to help
 * for a while. */
static void par_help(par_pool* p, cx_u32 w) {
    cx_u64 start = cx_clock_ns();
    for (unsigned spins = 1;; spins++) {
        if (__atomic_load_n(&p->top.word, __ATOMIC_ACQUIRE) != w) return;
        if (__atomic_load_n(&p->top.panic, __ATOMIC_RELAXED) != PAR_NONE) return;
        cx_u32 seen = __atomic_load_n(&p->help_word, __ATOMIC_SEQ_CST);
        if (par_help_once(p)) {
            start = cx_clock_ns();
            continue;
        }
        par_pause();
        if (spins % 64 || cx_clock_ns() - start <= PAR_SPIN_NS) continue;
        __atomic_fetch_add(&p->help_sleepers, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->help_word, __ATOMIC_SEQ_CST) == seen && __atomic_load_n(&p->top.word, __ATOMIC_SEQ_CST) == w) {
            par_sleep(&p->help_word, seen);
        }
        __atomic_fetch_sub(&p->help_sleepers, 1, __ATOMIC_RELAXED);
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

/* After a panic in `p`'s top job, on the thread that owns it: once every chunk before the first
 * one that panicked has run, writes their output and that chunk's, which ends with the panic
 * message and the frames inside the loop, then the frames outside it, and exits. The job stays
 * open until then, since a range of chunks before the one that panicked may belong to a worker
 * that hasn't come yet. */
static CX_COLD void par_end_panic(par_pool* p) {
    take_exit();
    cx_u64 first;
    for (unsigned spins = 0;; spins++) {
        first = __atomic_load_n(&p->top.panic, __ATOMIC_ACQUIRE);
        cx_u64 c = 0;
        while (c < first && __atomic_load_n(&p->top.done[c], __ATOMIC_ACQUIRE)) c++;
        if (c == first) break;
        if (spins < 4096) par_pause();
        else par_yield();
    }
    par_close(&p->top);
    par_cur = 0;
    par_buffered = 0;
    to_stderr = 0;
    par_replay(&p->top, first + 1);
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
    par_pool* p = j->pool;
    par_close(j);
    __atomic_fetch_and(&p->open, ~((cx_u64)1 << j->slot), __ATOMIC_RELEASE);
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
    __atomic_fetch_and(&p->taken, ~((cx_u64)1 << j->slot), __ATOMIC_RELEASE);
    par_panicked();
}

static CX_COLD void par_panicked(void) {
    par_job* j = par_cur;
    par_pool* p = j->pool;
    cx_u64 c = par_chunk;
    cx_u64 first = __atomic_load_n(&j->panic, __ATOMIC_ACQUIRE);
    while (c < first && !__atomic_compare_exchange_n(&j->panic, &first, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (par_owner) {
        if (j == &p->top) par_end_panic(p);
        par_end_inner(j);
    }
    /* A helper leaves the job, and its owner takes the panic from there. */
    if (j == &p->top) __atomic_store_n(&par_mine->in, 0, __ATOMIC_RELEASE);
    else __atomic_fetch_sub(&j->active, 1, __ATOMIC_RELEASE);
    if (!par_worker) {
        /* The thread that owns the pool's top job, helping an inner job: the panic reaches the
         * top job through the inner job's owner, and this thread ends the program then. */
        while (__atomic_load_n(&p->top.panic, __ATOMIC_ACQUIRE) == PAR_NONE) par_yield();
        par_end_panic(p);
    }
    /* A worker stops for good; the top job's owner ends the program. */
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

/* Waits until `p`'s top job's word isn't `seen`. */
static void par_wait(par_pool* p, cx_u32 seen) {
    cx_u64 start = cx_clock_ns();
    for (unsigned spins = 1;; spins++) {
        if (__atomic_load_n(&p->top.word, __ATOMIC_ACQUIRE) != seen) return;
        par_pause();
        if (spins % 64 == 0 && cx_clock_ns() - start > PAR_SPIN_NS) break;
    }
    __atomic_fetch_add(&p->sleepers, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&p->top.word, __ATOMIC_SEQ_CST) == seen) par_sleep(&p->top.word, seen);
    __atomic_fetch_sub(&p->sleepers, 1, __ATOMIC_RELAXED);
}

static void par_work(par_pool* p, cx_usize id, par_line* mine) {
    par_worker = 1;
    par_slot = (cx_u32)id + 1;
    par_mine = mine;
    cx_u32 seen = 0;
    for (;;) {
        par_wait(p, seen);
        if (__atomic_load_n(&p->stop, __ATOMIC_SEQ_CST)) break;
        cx_u32 w = __atomic_load_n(&p->top.word, __ATOMIC_SEQ_CST);
        seen = w;
        if (!(w & 1)) continue; /* a job that closed before this worker saw it */
        if (id + 1 >= p->target) continue; /* a loop that runs on fewer threads */
        cx_u64 in = (cx_u64)w << 32;
        __atomic_store_n(&mine->in, in, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->top.word, __ATOMIC_SEQ_CST) == w) {
            par_depth = 1;
            cx_u64 ran = par_claim(&p->top, 0, 0);
            /* Release, so the owner sees what the chunks wrote. */
            __atomic_store_n(&mine->in, in + 1 + ran, __ATOMIC_RELEASE);
            par_help(p, w);
            par_depth = 0;
        }
        __atomic_store_n(&mine->in, 0, __ATOMIC_RELEASE);
    }
    __atomic_fetch_sub(&p->alive, 1, __ATOMIC_SEQ_CST);
}

/* What a worker thread starts with. */
typedef struct {
    par_pool* p;
    cx_usize id;
    par_line* line;
} par_start;

static void par_begin(void* arg) {
    par_start s = *(par_start*)arg;
    free(arg);
    par_work(s.p, s.id, s.line);
}

#ifdef _WIN32
static DWORD WINAPI par_thread(LPVOID arg) {
    par_begin(arg);
    return 0;
}
#else
static void* par_thread(void* arg) {
    par_begin(arg);
    return 0;
}
#endif

/* The threads each loop on the main pool uses: the number of logical processors, unless
 * `set_workers` changed it. */
static cx_usize par_threads(void) {
    if (!par_main.target) {
#ifdef _WIN32
        long n = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        cpu_set_t set;
        long n = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : sysconf(_SC_NPROCESSORS_ONLN);
#endif
        par_main.target = n > 0 ? (cx_usize)n : 1;
    }
    return par_main.target;
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
    par_main.target = n;
}

static void* par_aligned(size_t size) {
#ifdef _WIN32
    void* q = _aligned_malloc(size, 128);
#else
    void* q = aligned_alloc(128, size);
#endif
    if (!q) par_fail("panic at <memory>: out of memory\n");
    return q;
}

static void par_aligned_free(void* q) {
#ifdef _WIN32
    _aligned_free(q);
#else
    free(q);
#endif
}

static void par_start_workers(par_pool* p, cx_usize threads) {
    while (p->started + 1 < threads) {
        if (p->started == p->lines_cap) {
            cx_usize cap = p->lines_cap ? p->lines_cap * 2 : 16;
            par_line** lines = realloc(p->lines, cap * sizeof *lines);
            if (!lines) par_fail("panic at <memory>: out of memory\n");
            p->lines = lines;
            p->lines_cap = cap;
        }
        par_line* line = par_aligned(sizeof(par_line));
        line->in = 0;
        p->lines[p->started] = line;
        par_start* s = malloc(sizeof *s);
        if (!s) par_fail("panic at <memory>: out of memory\n");
        s->p = p;
        s->id = p->started;
        s->line = line;
        __atomic_fetch_add(&p->alive, 1, __ATOMIC_SEQ_CST);
#ifdef _WIN32
        HANDLE h;
        if (cx_os_thread(&h, par_thread, s)) par_fail("panic at <parallel>: can't start a worker thread\n");
        CloseHandle(h);
#else
        pthread_t t;
        if (cx_os_thread(&t, par_thread, s)) par_fail("panic at <parallel>: can't start a worker thread\n");
        pthread_detach(t);
#endif
        p->started++;
    }
}

/* Sets up `j` of `p` for iterations `base` to `base + n` of `body`, as its owner, on `threads`
 * threads: with its chunks split evenly between their ranges if `spread`, or all in its owner's. */
static void par_setup(par_pool* p, par_job* j, cx_u64 base, cx_u64 n, par_body body, void* ctx, cx_usize threads, int spread) {
    cx_u64 chunk = n / ((cx_u64)threads * PAR_CHUNKS);
    if (chunk == 0) chunk = 1;
    cx_u64 chunks = n / chunk + (n % chunk != 0);
    par_size(j, chunks);
    if (threads > j->ranges_cap) {
        if (j->ranges) par_aligned_free(j->ranges);
        j->ranges = par_aligned(threads * sizeof(par_range));
        j->ranges_cap = (cx_u32)threads;
    }
    j->slots = (cx_u32)threads;
    for (cx_u32 t = 0; t < j->slots; t++) {
        cx_u64 w = 0;
        /* Rounded up, so the owner's range has a chunk even with fewer chunks than threads, and
         * the owner measures the loop. */
        if (spread) w = PAR_RANGE((chunks * t + threads - 1) / threads, (chunks * (t + 1) + threads - 1) / threads);
        else if (t == par_slot) w = PAR_JOINED | PAR_RANGE(0, chunks);
        __atomic_store_n(&j->ranges[t].word, w, __ATOMIC_RELAXED);
    }
    j->pool = p;
    j->body = body;
    j->ctx = ctx;
    j->base = base;
    j->n = n;
    j->chunk = chunk;
    j->chunks = chunks;
    j->panic = PAR_NONE;
}

/* Iterations `base` to `base + n` of a loop inside a chunk of `p`, on a slot of its own that idle
 * threads help with. Returns 0 if every slot is taken. Never inlined, so a stack trace finds
 * where the loop starts. */
static __attribute__((noinline)) int par_inner(par_pool* p, cx_u64 base, cx_u64 n, par_body body, void* ctx, cx_usize threads, par_meter* m) {
    cx_u64 taken = __atomic_load_n(&p->taken, __ATOMIC_RELAXED);
    unsigned k;
    do {
        if (!~taken) return 0;
        k = (unsigned)__builtin_ctzll(~taken);
    } while (!__atomic_compare_exchange_n(&p->taken, &taken, taken | ((cx_u64)1 << k), 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    par_job* j = &p->nest[k];
    par_setup(p, j, base, n, body, ctx, threads, 0);
    j->slot = k;
    j->up = par_cur;
    j->up_chunk = par_chunk;
    j->up_owner = par_owner;
    __atomic_store_n(&j->word, j->word + 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&p->open, (cx_u64)1 << k, __ATOMIC_RELEASE);
    par_nudge(p);

    par_claim(j, 1, m);
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_inner(j);
    par_close(j);
    __atomic_fetch_and(&p->open, ~((cx_u64)1 << k), __ATOMIC_RELEASE);
    par_drain(j);
    /* A helper's chunk may have panicked after the owner's last check. */
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_inner(j);
    par_replay(j, j->chunks);
    __atomic_fetch_and(&p->taken, ~((cx_u64)1 << k), __ATOMIC_RELEASE);
    return 1;
}

/* Runs iterations of `body` from 0 on this thread, in batches that double, while what's left
 * looks like less than `PAR_ALONE_US` of work at the rate so far. Returns where it stopped, `n`
 * once it has run them all, and keeps the rate. */
static cx_u64 par_alone(par_site* site, cx_u64 n, par_body body, void* ctx) {
    cx_u64 start = par_ticks(), done = 0, ticks;
    par_depth++;
    for (cx_u64 batch = 1;;) {
        cx_u64 end = n - done < batch ? n : done + batch;
        if (batch < ((cx_u64)1 << 62)) batch *= 2;
        body(ctx, done, end);
        done = end;
        ticks = par_ticks() - start;
        if (done == n) break;
        /* What's left at the rate so far, ticks * (n - done) / done, once the clock has counted
         * enough to tell. */
        if (ticks < 2) continue;
        cx_u64 left, limit;
        if (__builtin_mul_overflow(ticks, n - done, &left)) break;
        if (__builtin_mul_overflow(par_tick_hz / 1000000u * PAR_ALONE_US, done, &limit) == 0 && left > limit) break;
    }
    par_depth--;
    par_learn(site, body, ticks, done);
    return done;
}

/* Iterations `base` to `n` of `body` as `p`'s top job, on `threads` threads. */
static void par_top(par_pool* p, cx_usize threads, cx_u64 base, cx_u64 n, par_body body, void* ctx, par_site* site) {
    par_meter m = {0, 0};
    par_start_workers(p, threads);
    par_job* j = &p->top;
    par_slot = 0;
    par_setup(p, j, base, n - base, body, ctx, threads, 1);
    j->up = 0;
    cx_u32 w = j->word + 1;
    __atomic_store_n(&j->word, w, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->sleepers, __ATOMIC_SEQ_CST)) par_wake(&j->word);

    int depth = par_depth;
    par_depth = depth + 1;
    cx_u64 ran = par_claim(j, 1, &m);
    /* Helps the chunks still running with their inner loops, until the workers that came have
     * run every chunk. */
    cx_usize workers = threads - 1;
    for (unsigned spins = 0;; spins++) {
        if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic(p);
        cx_u64 all = ran;
        for (cx_usize k = 0; k < workers; k++) {
            cx_u64 in = __atomic_load_n(&p->lines[k]->in, __ATOMIC_ACQUIRE);
            if (in >> 32 == w && (cx_u32)in) all += (cx_u32)in - 1;
        }
        if (all == j->chunks) break;
        if (par_help_once(p)) spins = 0;
        else if (spins < 4096) par_pause();
        else par_yield();
    }
    par_close(j);
    par_nudge(p);
    /* Waits for each worker still taking chunks to let go of the job, as the next loop sets its
     * ranges up again. One past its chunks only helps inner jobs, which have all ended, until it
     * sees the job close, so it isn't waited for: on a machine where other programs want a
     * processor, the system had often preempted one there, and the loop's end waited for it to
     * run again (CLX-267). */
    for (cx_usize k = 0; k < workers; k++) {
        for (unsigned spins = 0;; spins++) {
            cx_u64 in = __atomic_load_n(&p->lines[k]->in, __ATOMIC_SEQ_CST);
            if (in >> 32 != w || (cx_u32)in) break;
            if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic(p);
            if (spins < 4096) par_pause();
            else par_yield();
        }
    }
    par_depth = depth;
    if (__atomic_load_n(&j->panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic(p);
    par_replay(j, j->chunks);
    par_learn(site, body, m.ticks, m.iters);
}

/* `n` iterations of `body` on `p`, on `threads` threads: a loop of its own, or one inside a chunk
 * of `p`'s, or on this thread when it has one thread, one iteration, or every inner slot taken,
 * or looks like little work. */
static void par_run(par_pool* p, cx_usize threads, cx_u64 n, par_body body, void* ctx) {
    cx_u64 base = 0;
    par_site* site = 0;
    /* A loop inside one this thread runs alone, outside a chunk, runs on the same pool. */
    par_pool* home = par_home;
    if (!par_cur) par_home = p;
    if (threads > 1 && n > 1) {
        site = par_site_of(body);
        if (par_cheap(par_cost(site, body))) base = par_alone(site, n, body, ctx);
    }
    par_meter m = {0, 0};
    if (base == n) {
    } else if (threads <= 1 || n == 1 || (par_cur && !par_inner(p, base, n - base, body, ctx, threads, &m))) {
        par_depth++;
        body(ctx, base, n);
        par_depth--;
    } else if (par_cur) {
        par_learn(site, body, m.ticks, m.iters);
    } else {
        par_top(p, threads, base, n, body, ctx, site);
    }
    par_home = home;
}

/* Never inlined, so a stack trace finds where a loop starts. A loop inside a chunk runs on the
 * chunk's pool, and one inside a loop this thread runs alone on that loop's. */
__attribute__((noinline)) void cx_parallel(cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx) {
    if (n == 0) return;
    par_pool* p = par_cur ? par_cur->pool : par_home ? par_home : &par_main;
    /* The main pool is the main thread's, so a thread `Thread.spawn` started runs its loops alone,
     * unless it runs one on a pool of the program's. */
    cx_usize threads = p != &par_main ? p->target : par_service ? 1 : par_threads();
    par_run(p, threads, n, body, ctx);
}

/* Waits until no other thread runs a loop on `p`, and takes it. */
static void par_take(par_pool* p) {
    for (unsigned spins = 0;; spins++) {
        cx_u32 was = 0;
        if (__atomic_compare_exchange_n(&p->busy, &was, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spins < 4096) {
            par_pause();
            continue;
        }
        __atomic_fetch_add(&p->busy_sleepers, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->busy, __ATOMIC_SEQ_CST)) par_sleep(&p->busy, 1);
        __atomic_fetch_sub(&p->busy_sleepers, 1, __ATOMIC_RELAXED);
    }
}

static void par_give(par_pool* p) {
    __atomic_store_n(&p->busy, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->busy_sleepers, __ATOMIC_SEQ_CST)) par_wake(&p->busy);
}

/* `parallel(pool) for` (CLX-71): the loop on the program's pool `pool`, a `par_pool*` as a number
 * in std.pool's `Pool`. Inside a chunk it runs as any loop there does, on the chunk's pool, so
 * its output and its panics go where the chunk's do, and inside a loop this thread runs alone it
 * runs on that loop's pool. Never inlined, so a stack trace finds where the loop starts. */
__attribute__((noinline)) void cx_parallel_on(cx_u64 pool, cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx) {
    if (n == 0) return;
    if (par_cur || par_home) {
        cx_parallel(n, body, ctx);
        return;
    }
    par_pool* p = (par_pool*)(uintptr_t)pool;
    par_take(p);
    par_run(p, p->target, n, body, ctx);
    par_give(p);
}

/* A pool's memory, aligned for its shared counters. */
static par_pool* par_pool_alloc(void) {
    par_pool* p = par_aligned(sizeof(par_pool));
    memset(p, 0, sizeof *p);
    return p;
}

/* Declared by std/pool.calx, which checks that `threads` isn't 0: a pool whose loops run on
 * `threads` threads, counting the one that runs the loop. Its workers start with its first loop. */
cx_u64 cx_pool_new(cx_usize threads) {
    par_pool* p = par_pool_alloc();
    p->top.panic = PAR_NONE;
    p->target = threads;
    return (cx_u64)(uintptr_t)p;
}

/* Declared by std/pool.calx: once no loop runs on the pool, stops its workers, waits for them to
 * stop, and frees it. */
void cx_pool_close(cx_u64 pool) {
    par_pool* p = (par_pool*)(uintptr_t)pool;
    par_take(p);
    __atomic_store_n(&p->stop, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&p->top.word, 2, __ATOMIC_SEQ_CST);
    par_wake(&p->top.word);
    for (unsigned spins = 0; __atomic_load_n(&p->alive, __ATOMIC_SEQ_CST); spins++) {
        if (spins < 4096) par_pause();
        else par_yield();
    }
    free(p->top.done);
    free(p->top.outs);
    if (p->top.ranges) par_aligned_free(p->top.ranges);
    for (cx_usize k = 0; k < p->started; k++) par_aligned_free(p->lines[k]);
    free(p->lines);
    for (int k = 0; k < PAR_NESTED; k++) {
        free(p->nest[k].done);
        free(p->nest[k].outs);
        if (p->nest[k].ranges) par_aligned_free(p->nest[k].ranges);
    }
    par_aligned_free(p);
}
