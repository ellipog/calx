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
#endif

/* --- output ------------------------------------------------------------------------------- */

static unsigned char out_buf[1 << 16];
static size_t out_len;
static int to_stderr; /* while a panic message is being written */

static void flush_out(void) {
    if (out_len) {
        fwrite(out_buf, 1, out_len, stdout);
        out_len = 0;
    }
    fflush(stdout);
}

static void write_bytes(const void* p, size_t n) {
    if (to_stderr) {
        fwrite(p, 1, n, stderr);
        return;
    }
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

void cx_start(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    /* The same bytes on every platform: no "\n" to "\r\n" translation, and UTF-8 in a console. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

int cx_finish(void) {
    flush_out();
    return 0;
}

void cx_print_str(cx_str s) { write_bytes(s.ptr, s.len); }

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

/* --- panics -------------------------------------------------------------------------------- */

void cx_panic_begin(const char* at) {
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
