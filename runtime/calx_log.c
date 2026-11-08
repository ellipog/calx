/* std.log's settings, which the whole program shares, as Python's logging module's root logger
 * is shared: Calx has no global state outside `unsafe` (CALX_SPEC), so the level, the format, the
 * date format and the open log file are kept here, behind a lock, and std.log asks for them. The
 * records are put together in Calx. Linked only into programs that call it. */
#include "calx_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static SRWLOCK log_mutex = SRWLOCK_INIT;
static void log_lock(void) { AcquireSRWLockExclusive(&log_mutex); }
static void log_unlock(void) { ReleaseSRWLockExclusive(&log_mutex); }
#else
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void) { pthread_mutex_lock(&log_mutex); }
static void log_unlock(void) { pthread_mutex_unlock(&log_mutex); }
#endif

/* Whether the root logger has been set up, by `config` or by a root function's first call, which
 * sets it up as Python's basicConfig() does with nothing given. Read without the lock, as is the
 * level, so a record that's off costs two loads. */
static int log_ready;
static cx_i32 log_level = 30;
static cx_u8* log_format;
static cx_usize log_format_len;
static cx_u8* log_datefmt;
static cx_usize log_datefmt_len;
static FILE* log_file; /* NULL: standard error, which std.log writes itself */
static cx_i64 log_start_ms = -1;

static cx_u8* log_copy(const cx_u8* p, cx_usize n) {
    cx_u8* c = (cx_u8*)malloc(n ? n : 1);
    if (!c) abort();
    if (n) memcpy(c, p, n);
    return c;
}

static FILE* log_open(const cx_u8* path, cx_usize n, int append) {
#ifdef _WIN32
    if (n > 0x7FFFFFFF) return NULL;
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path, (int)n, NULL, 0);
    if (units == 0) return NULL;
    wchar_t* w = (wchar_t*)malloc(((size_t)units + 1) * sizeof(wchar_t));
    if (!w) abort();
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)path, (int)n, w, units);
    w[units] = 0;
    FILE* f = _wfopen(w, append ? L"ab" : L"wb");
    free(w);
    return f;
#else
    char* c = (char*)malloc(n + 1);
    if (!c) abort();
    memcpy(c, path, n);
    c[n] = 0;
    FILE* f = memchr(c, 0, n) ? NULL : fopen(c, append ? "ab" : "wb");
    free(c);
    return f;
#endif
}

/* Sets the root logger up, replacing what was set before, and writes to the file at `path`, or to
 * standard error when it's empty. 0, or -1 when the file can't be opened, which leaves the last
 * settings as they were. */
cx_i32 cx_log_config(cx_i32 level, const cx_u8* format, cx_usize format_len, const cx_u8* datefmt, cx_usize datefmt_len,
                     const cx_u8* path, cx_usize path_len, cx_bool append) {
    FILE* f = NULL;
    if (path_len) {
        f = log_open(path, path_len, append);
        if (!f) return -1;
    }
    log_lock();
    if (log_file) fclose(log_file);
    log_file = f;
    free(log_format);
    log_format = log_copy(format, format_len);
    log_format_len = format_len;
    free(log_datefmt);
    log_datefmt = log_copy(datefmt, datefmt_len);
    log_datefmt_len = datefmt_len;
    __atomic_store_n(&log_level, level, __ATOMIC_RELEASE);
    __atomic_store_n(&log_ready, 1, __ATOMIC_RELEASE);
    log_unlock();
    return 0;
}

cx_bool cx_log_ready(void) { return __atomic_load_n(&log_ready, __ATOMIC_ACQUIRE) != 0; }

cx_i32 cx_log_level(void) { return __atomic_load_n(&log_level, __ATOMIC_ACQUIRE); }

void cx_log_set_level(cx_i32 level) { __atomic_store_n(&log_level, level, __ATOMIC_RELEASE); }

/* Copies the format, or the date format when `which` is 1, into `out` if it fits in `cap`, and
 * gives its length either way. */
cx_usize cx_log_text(cx_u32 which, cx_u8* out, cx_usize cap) {
    log_lock();
    const cx_u8* p = which ? log_datefmt : log_format;
    cx_usize n = which ? log_datefmt_len : log_format_len;
    if (n <= cap && n) memcpy(out, p, n);
    log_unlock();
    return n;
}

/* Whether records go to a file, which `cx_log_write` writes, rather than to standard error. */
cx_bool cx_log_to_file(void) {
    log_lock();
    int f = log_file != NULL;
    log_unlock();
    return f;
}

/* Adds a record's line to the log file, and flushes it, as Python's FileHandler does, so a line is
 * in the file once the call returns. */
void cx_log_write(const cx_u8* p, cx_usize n) {
    log_lock();
    if (log_file) {
        if (n) fwrite(p, 1, n, log_file);
        fflush(log_file);
    }
    log_unlock();
}

/* The milliseconds from the first time std.log asked, which is as the program starts for any that
 * logs early: Python's relativeCreated counts from when logging was imported. */
cx_i64 cx_log_start_ms(cx_i64 now) {
    cx_i64 none = -1;
    __atomic_compare_exchange_n(&log_start_ms, &none, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&log_start_ms, __ATOMIC_ACQUIRE);
}

cx_u32 cx_log_pid(void) {
#ifdef _WIN32
    return (cx_u32)GetCurrentProcessId();
#else
    return (cx_u32)getpid();
#endif
}
