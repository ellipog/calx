/* The Calx runtime's compiled part. The driver builds this once per target and build mode and
 * links it into every program. */
#include "calx_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

/* --- output ------------------------------------------------------------------------------- */

static unsigned char out_buf[1 << 16];
static size_t out_len;
static int to_stderr; /* while a panic message is being written */
static _Thread_local cx_String* fmt_to; /* while a string is being interpolated */

#ifdef CX_DEBUG
/* Debug builds count the allocations still live, and a program that ends with any left has
 * leaked: the compiler drops every value that owns memory (D76), so that's a compiler bug. */
static cx_usize live_allocs;
#endif

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
    } else if (to_stderr) {
        fwrite(p, 1, n, stderr);
    } else {
        out_bytes(p, n);
    }
}

void cx_start(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    /* The same bytes on every platform: no "\n" to "\r\n" translation, and UTF-8 in a console. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
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
    if (out_len) flush_out();
    to_stderr = 1;
}

void cx_err_end(void) { to_stderr = 0; }

void cx_out_write(const cx_u8* p, cx_usize n) { out_bytes(p, n); }

void cx_out_flush(void) { flush_out(); }

/* Reads at most `n` bytes, returning 0 only at the end of the input. Output waiting in the
 * buffer is written first, since the program may be waiting for an answer to it. */
cx_usize cx_in_read(cx_u8* p, cx_usize n) {
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
        fputs("can't read standard input", stderr);
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
    live_allocs++;
#endif
    return p;
}

void* cx_realloc(void* p, cx_usize size) {
#ifdef CX_DEBUG
    if (!p) live_allocs++;
#endif
    void* q = realloc(p, size ? size : 1);
    if (!q) cx_panic_capacity();
    return q;
}

void cx_free(void* p) {
#ifdef CX_DEBUG
    if (p) live_allocs--;
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

/* --- panics -------------------------------------------------------------------------------- */

void cx_panic_begin(const char* at) {
    fmt_to = 0;
    flush_out();
    to_stderr = 1;
    fputs("panic at ", stderr);
    fputs(at, stderr);
    fputs(": ", stderr);
}

void cx_panic_end(void) {
    fputc('\n', stderr);
    fflush(stderr);
    exit(101);
}

void cx_panic_index(cx_usize index, cx_usize length, const char* at) {
    cx_panic_begin(at);
    fprintf(stderr, "index %llu is out of bounds for length %llu", (unsigned long long)index,
            (unsigned long long)length);
    cx_panic_end();
}

void cx_panic_div_zero(const char* at) {
    cx_panic_begin(at);
    fputs("division by zero", stderr);
    cx_panic_end();
}

void cx_panic_capacity(void) {
    cx_panic_begin("<memory>");
    fputs("out of memory", stderr);
    cx_panic_end();
}

void cx_panic_insert(cx_usize index, cx_usize length, const char* at) {
    cx_panic_begin(at);
    fprintf(stderr, "can't insert at index %llu in a list of length %llu", (unsigned long long)index,
            (unsigned long long)length);
    cx_panic_end();
}

void cx_panic_set_len(cx_usize length, cx_usize capacity, const char* at) {
    cx_panic_begin(at);
    fprintf(stderr, "can't set the length to %llu in a list with capacity %llu", (unsigned long long)length,
            (unsigned long long)capacity);
    cx_panic_end();
}

void cx_panic_unwrap(const char* at) {
    cx_panic_begin(at);
    fputs("unwrapped `none` with `!`", stderr);
    cx_panic_end();
}
