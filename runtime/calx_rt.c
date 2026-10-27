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
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

/* --- output ------------------------------------------------------------------------------- */

/* Standard output's buffer belongs to the main thread. Inside a `parallel for`, every thread
 * writes to the buffer of the chunk it's running instead (see "parallel for" below). */
static unsigned char out_buf[1 << 16];
static size_t out_len;
static _Thread_local int to_stderr; /* while `eprint` or a panic message is being written */
static _Thread_local cx_String* fmt_to; /* while a string is being interpolated */

#ifdef CX_DEBUG
/* Debug builds count the allocations still live, and a program that ends with any left has
 * leaked: the compiler drops every value that owns memory (D76), so that's a compiler bug. */
static cx_usize live_allocs;
#define COUNT_ALLOC(d) __atomic_fetch_add(&live_allocs, (cx_usize)(d), __ATOMIC_RELAXED)
#endif

static _Thread_local int par_buffered; /* output goes to the running chunk's buffer */
static _Thread_local int par_depth;    /* inside a `parallel for`, even one run on this thread alone */
static void par_write(int stream, const void* p, size_t n);

static void flush_out(void) {
    if (out_len) {
        fwrite(out_buf, 1, out_len, stdout);
        out_len = 0;
    }
    fflush(stdout);
}

static void out_bytes(const void* p, size_t n) {
    if (n == 0) return; /* an empty string's pointer can be null, which memcpy can't take */
    if (n > sizeof out_buf - out_len) {
        flush_out();
        if (n > sizeof out_buf) {
            fwrite(p, 1, n, stdout);
            return;
        }
    }
    memcpy(out_buf + out_len, p, n);
    out_len += n;
}

static void write_bytes(const void* p, size_t n) {
    if (n == 0) return;
    if (fmt_to) {
        cx_string_push_bytes(fmt_to, (const cx_u8*)p, n);
    } else if (par_buffered) {
        par_write(to_stderr, p, n);
    } else if (to_stderr) {
        fwrite(p, 1, n, stderr);
    } else {
        out_bytes(p, n);
    }
}

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
}

int cx_finish(void) {
    flush_out();
#ifdef CX_DEBUG
    if (live_allocs) {
        fprintf(stderr, "calx: %llu allocations were never freed; this is a bug in calx, please report it\n",
                (unsigned long long)live_allocs);
        return 102;
    }
#endif
    return 0;
}

void cx_print_str(cx_str s) { write_bytes(s.ptr, s.len); }

/* --- std.io --------------------------------------------------------------------------------
 * These are declared by std/io.calx as C functions, not in calx_rt.h. */

void cx_err_begin(void) {
    if (out_len && !par_buffered) flush_out();
    to_stderr = 1;
}

void cx_err_end(void) { to_stderr = 0; }

void cx_out_write(const cx_u8* p, cx_usize n) {
    if (par_buffered) par_write(0, p, n);
    else out_bytes(p, n);
}

/* Inside a `parallel for`, output is written when the loop ends, in order. */
void cx_out_flush(void) {
    if (!par_buffered) flush_out();
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

void* cx_alloc(cx_usize size) {
    void* p = malloc(size ? size : 1);
    if (!p) cx_panic_capacity();
#ifdef CX_DEBUG
    COUNT_ALLOC(1);
#endif
    return p;
}

void* cx_realloc(void* p, cx_usize size) {
#ifdef CX_DEBUG
    if (!p) COUNT_ALLOC(1);
#endif
    void* q = realloc(p, size ? size : 1);
    if (!q) cx_panic_capacity();
    return q;
}

void cx_free(void* p) {
#ifdef CX_DEBUG
    if (p) COUNT_ALLOC(-1);
#endif
    free(p);
}

void* cx_grow(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    /* No buffer can be bigger than half the address space. */
    cx_usize max = ((cx_usize)-1 / 2) / (size ? size : 1);
    if (extra > max - len) cx_panic_capacity();
    cx_usize need = len + extra;
    if (need <= *cap) return ptr;
    cx_usize n = *cap <= max / 2 ? *cap * 2 : max;
    if (n < need) n = need;
    if (n < 4 && max >= 4) n = 4;
    ptr = cx_realloc(ptr, n * size);
    *cap = n;
    return ptr;
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
    s->ptr = (cx_u8*)cx_realloc(s->ptr, n);
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

cx_bool cx_str_contains(cx_str hay, cx_str needle) {
    if (needle.len == 0) return 1;
    if (needle.len > hay.len) return 0;
    for (cx_usize i = 0; i + needle.len <= hay.len; i++) {
        if (hay.ptr[i] == needle.ptr[0] && memcmp(hay.ptr + i, needle.ptr, needle.len) == 0) return 1;
    }
    return 0;
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
    if (!par_buffered) flush_out();
    to_stderr = 1;
    panic_text("panic at ");
    panic_text(at);
    panic_text(": ");
}

void cx_panic_end(void) {
    panic_text("\n");
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

/* --- parallel for ---------------------------------------------------------------------------
 * `parallel for` runs its body on a pool of threads that the calling thread takes part in. The
 * iterations are split into chunks, which threads claim in order from a shared counter. The
 * checker makes sure no two iterations touch the same memory (D21), so what they compute can't
 * depend on which thread ran what. What they print is kept per chunk and written in chunk order
 * when the loop ends, and a panic reports the first iteration that panicked, after the output of
 * every iteration before it. So the output is the same with one thread and with many, too.
 *
 * Workers wait for a job on `par_word`, which is odd while a job is open: they spin for a while,
 * since loops often come one after another, then sleep on it. A worker joins a job by counting
 * itself in `par_active` and then checking that the word hasn't changed. The calling thread
 * closes the job once every chunk is claimed and waits for `par_active` to reach 0, so no worker
 * reads a job after that, and a sleeping worker is never waited for. Only the main thread starts
 * jobs: a `parallel for` inside another runs on the thread that reaches it. */

#define PAR_NONE (~(cx_u64)0)
#define PAR_SPIN_NS 200000u /* how long a worker spins before it sleeps */
#define PAR_CHUNKS 8        /* chunks per thread, so a slow chunk doesn't hold the rest up */
#define PAR_SHARED __attribute__((aligned(128))) /* its own pair of cache lines */

typedef void (*par_body)(void* ctx, cx_u64 begin, cx_u64 end);

/* A chunk's output: segments of one byte naming the stream (1 for standard error), the length,
 * and the bytes. */
typedef struct {
    unsigned char* p;
    size_t len, cap;
    size_t last; /* where the last segment starts, or SIZE_MAX */
} par_out;

/* The open job. The main thread writes it before opening the job, and nothing changes it then. */
static struct {
    par_body body;
    void* ctx;
    cx_u64 n, chunk, chunks;
    cx_usize threads; /* how many threads may take part, the main thread included */
} job;
static unsigned char* par_done; /* per chunk: whether it has run, for a panic */
static par_out** par_outs;      /* per chunk: what it printed, once it prints */
static cx_u64 par_cap;          /* the length of both */
static cx_u32 par_jobs;         /* jobs started */
static cx_usize par_target;     /* threads to use, the main thread included; 0 until set or read */
static cx_usize par_started;    /* worker threads started */

static PAR_SHARED cx_u32 par_word;      /* jobs * 2, plus 1 while one is open */
static PAR_SHARED cx_u64 par_next;      /* the next chunk to claim */
static PAR_SHARED cx_u32 par_active;    /* workers in the open job */
static PAR_SHARED cx_u32 par_sleepers;  /* workers asleep on par_word */
static PAR_SHARED cx_u64 par_panic = PAR_NONE; /* the first chunk that panicked */

static _Thread_local cx_u64 par_chunk; /* the chunk this thread is running */
static _Thread_local int par_worker;   /* this is a worker thread */

static void par_pause(void) { __builtin_ia32_pause(); }

#ifdef _WIN32
static void par_sleep(cx_u32 seen) { WaitOnAddress(&par_word, &seen, sizeof seen, INFINITE); }
static void par_wake(void) { WakeByAddressAll(&par_word); }
static void par_yield(void) { SwitchToThread(); }
#else
static void par_sleep(cx_u32 seen) { syscall(SYS_futex, &par_word, FUTEX_WAIT_PRIVATE, seen, NULL, NULL, 0); }
static void par_wake(void) { syscall(SYS_futex, &par_word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0); }
static void par_yield(void) { sched_yield(); }
#endif

static CX_COLD void par_fail(const char* what) {
    fputs(what, stderr);
    fflush(stderr);
    _Exit(101);
}

static void par_write(int stream, const void* p, size_t n) {
    par_out* o = par_outs[par_chunk];
    if (!o) {
        o = calloc(1, sizeof *o);
        if (!o) par_fail("panic at <memory>: out of memory\n");
        o->last = SIZE_MAX;
        par_outs[par_chunk] = o;
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

/* Writes what chunks [0, upto) printed, in order, as if they'd printed it themselves. */
static void par_replay(cx_u64 upto) {
    for (cx_u64 c = 0; c < upto; c++) {
        par_out* o = par_outs[c];
        if (!o) continue;
        for (size_t at = 0; at < o->len;) {
            int stream = o->p[at];
            size_t len;
            memcpy(&len, o->p + at + 1, sizeof len);
            at += 1 + sizeof len;
            if (stream) {
                flush_out();
                fwrite(o->p + at, 1, len, stderr);
            } else {
                out_bytes(o->p + at, len);
            }
            at += len;
        }
        free(o->p);
        free(o);
        par_outs[c] = 0;
    }
}

/* Claims chunks of the open job and runs them until none are left. */
static void par_run(void) {
    par_buffered = 1;
    for (;;) {
        if (__atomic_load_n(&par_panic, __ATOMIC_RELAXED) != PAR_NONE) break;
        cx_u64 c = __atomic_fetch_add(&par_next, 1, __ATOMIC_RELAXED);
        if (c >= job.chunks) break;
        par_chunk = c;
        cx_u64 begin = c * job.chunk;
        cx_u64 end = job.n - begin < job.chunk ? job.n : begin + job.chunk;
        job.body(job.ctx, begin, end);
        __atomic_store_n(&par_done[c], 1, __ATOMIC_RELEASE);
    }
    par_buffered = 0;
}

/* After a panic, on the main thread: once every chunk before the first one that panicked has
 * run, writes their output and that chunk's, which ends with the panic message, and exits. */
static CX_COLD void par_end_panic(void) {
    __atomic_store_n(&par_word, par_jobs * 2, __ATOMIC_SEQ_CST);
    cx_u64 first;
    for (unsigned spins = 0;; spins++) {
        first = __atomic_load_n(&par_panic, __ATOMIC_ACQUIRE);
        cx_u64 c = 0;
        while (c < first && __atomic_load_n(&par_done[c], __ATOMIC_ACQUIRE)) c++;
        if (c == first) break;
        if (spins < 4096) par_pause();
        else par_yield();
    }
    par_buffered = 0;
    to_stderr = 0;
    par_replay(first + 1);
    flush_out();
    fflush(stderr);
    _Exit(101);
}

static CX_COLD void par_panicked(void) {
    cx_u64 c = par_chunk;
    cx_u64 first = __atomic_load_n(&par_panic, __ATOMIC_ACQUIRE);
    while (c < first && !__atomic_compare_exchange_n(&par_panic, &first, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (!par_worker) par_end_panic();
    /* A worker leaves the job and stops for good; the main thread ends the program. */
    __atomic_fetch_sub(&par_active, 1, __ATOMIC_RELEASE);
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

/* Waits until `par_word` isn't `seen`. */
static void par_wait(cx_u32 seen) {
    cx_u64 start = cx_clock_ns();
    for (unsigned spins = 1;; spins++) {
        if (__atomic_load_n(&par_word, __ATOMIC_ACQUIRE) != seen) return;
        par_pause();
        if (spins % 64 == 0 && cx_clock_ns() - start > PAR_SPIN_NS) break;
    }
    __atomic_fetch_add(&par_sleepers, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&par_word, __ATOMIC_SEQ_CST) == seen) par_sleep(seen);
    __atomic_fetch_sub(&par_sleepers, 1, __ATOMIC_RELAXED);
}

static void par_work(cx_usize id) {
    par_worker = 1;
    cx_u32 seen = 0;
    for (;;) {
        par_wait(seen);
        cx_u32 w = __atomic_load_n(&par_word, __ATOMIC_SEQ_CST);
        seen = w;
        if (!(w & 1)) continue; /* a job that closed before this worker saw it */
        __atomic_fetch_add(&par_active, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&par_word, __ATOMIC_SEQ_CST) == w && id + 1 < job.threads) {
            par_depth = 1;
            par_run();
            par_depth = 0;
        }
        __atomic_fetch_sub(&par_active, 1, __ATOMIC_RELEASE);
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

/* Declared by std/pool.calx, which checks that `n` isn't 0. */
void cx_set_workers(cx_usize n) {
    if (par_depth) {
        cx_panic_begin("std.pool.set_workers");
        panic_text("can't change the number of workers inside a `parallel for`");
        cx_panic_end();
    }
    par_target = n;
}

void cx_parallel(cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx) {
    if (n == 0) return;
    cx_usize threads = par_threads();
    if (par_depth || threads <= 1 || n == 1) {
        par_depth++;
        body(ctx, 0, n);
        par_depth--;
        return;
    }
    while (par_started + 1 < threads) {
        uintptr_t id = par_started;
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, par_thread, (LPVOID)id, 0, NULL);
        if (!h) par_fail("panic at <parallel>: can't start a worker thread\n");
        CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, par_thread, (void*)id)) par_fail("panic at <parallel>: can't start a worker thread\n");
        pthread_detach(t);
#endif
        par_started++;
    }
    cx_u64 chunk = n / ((cx_u64)threads * PAR_CHUNKS);
    if (chunk == 0) chunk = 1;
    cx_u64 chunks = n / chunk + (n % chunk != 0);
    if (chunks > par_cap) {
        free(par_done);
        free(par_outs);
        par_done = malloc(chunks);
        par_outs = calloc(chunks, sizeof *par_outs);
        if (!par_done || !par_outs) par_fail("panic at <memory>: out of memory\n");
        par_cap = chunks;
    }
    memset(par_done, 0, chunks);
    job.body = body;
    job.ctx = ctx;
    job.n = n;
    job.chunk = chunk;
    job.chunks = chunks;
    job.threads = threads;
    __atomic_store_n(&par_next, 0, __ATOMIC_RELAXED);
    par_jobs++;
    __atomic_store_n(&par_word, par_jobs * 2 + 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&par_sleepers, __ATOMIC_SEQ_CST)) par_wake();

    par_depth = 1;
    par_run();
    __atomic_store_n(&par_word, par_jobs * 2, __ATOMIC_SEQ_CST);
    for (unsigned spins = 0; __atomic_load_n(&par_active, __ATOMIC_SEQ_CST); spins++) {
        if (__atomic_load_n(&par_panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic();
        if (spins < 4096) par_pause();
        else par_yield();
    }
    par_depth = 0;
    if (__atomic_load_n(&par_panic, __ATOMIC_ACQUIRE) != PAR_NONE) par_end_panic();
    par_replay(chunks);
}
