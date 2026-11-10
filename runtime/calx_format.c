/* `{x:spec}` for integers, text and runes: a fill, an alignment and a width, a sign, a base and
 * its prefix, zeros after the sign, and digits in groups (CLX-93). Floats are in calx_float.c,
 * which writes its numbers through `cx_format_number` here. Linked only into programs that use a
 * format.
 *
 * Widths count runes. Everything goes out through `cx_print_str`, so a format writes into a
 * `String` being built as it does to the output, and a value is never put together in memory of
 * its own unless it's longer than the stack buffer. */
#include "calx_rt.h"

#include <string.h>

static int utf8_put(cx_u8* b, cx_u32 c) {
    if (c < 0x80) {
        b[0] = (cx_u8)c;
        return 1;
    }
    if (c < 0x800) {
        b[0] = (cx_u8)(0xC0 | c >> 6);
        b[1] = (cx_u8)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        b[0] = (cx_u8)(0xE0 | c >> 12);
        b[1] = (cx_u8)(0x80 | (c >> 6 & 0x3F));
        b[2] = (cx_u8)(0x80 | (c & 0x3F));
        return 3;
    }
    b[0] = (cx_u8)(0xF0 | c >> 18);
    b[1] = (cx_u8)(0x80 | (c >> 12 & 0x3F));
    b[2] = (cx_u8)(0x80 | (c >> 6 & 0x3F));
    b[3] = (cx_u8)(0x80 | (c & 0x3F));
    return 4;
}

/* The fill rune n times into out, which has room; the bytes written. */
static cx_usize fill_into(cx_u8* out, cx_u32 fill, cx_usize n) {
    if (fill < 0x80) {
        memset(out, (int)fill, n);
        return n;
    }
    cx_u8 b[4];
    cx_usize len = (cx_usize)utf8_put(b, fill);
    for (cx_usize i = 0; i < n; i++) memcpy(out + i * len, b, len);
    return n * len;
}

static cx_usize fill_bytes(cx_u32 fill) { return fill < 0x80 ? 1 : fill < 0x800 ? 2 : fill < 0x10000 ? 3 : 4; }

/* The fill rune, n times, for padding too long for one buffer. */
static void put_fill(cx_u32 fill, cx_usize n) {
    cx_u8 buf[64];
    cx_usize per = 64 / fill_bytes(fill);
    while (n) {
        cx_usize k = n < per ? n : per;
        cx_print_str((cx_str){buf, fill_into(buf, fill, k)});
        n -= k;
    }
}

/* Most formatted values fit here, and go out in one write. */
#define SMALL 256

static cx_usize rune_count(cx_str s) {
    cx_usize n = 0;
    for (cx_usize i = 0; i < s.len; i++) n += (s.ptr[i] & 0xC0) != 0x80;
    return n;
}

/* `left` and `right`, the fill before and after `runes` runes, by the alignment, or `def` when the
 * format gives none. A centred value has the odd one after it. */
static void split_pad(cx_spec s, cx_usize runes, char def, cx_usize* left, cx_usize* right) {
    cx_usize pad = s.width > runes ? s.width - runes : 0;
    char align = s.align ? (char)s.align : def;
    *left = align == '<' ? 0 : align == '>' ? pad : pad / 2;
    *right = pad - *left;
}

void cx_format_str(cx_str v, cx_spec s) {
    if (s.precision >= 0) {
        cx_usize runes = 0, i = 0;
        while (i < v.len) {
            if ((v.ptr[i] & 0xC0) != 0x80) {
                if (runes == (cx_usize)s.precision) break;
                runes++;
            }
            i++;
        }
        v.len = i;
    }
    cx_usize left, right;
    split_pad(s, s.width ? rune_count(v) : 0, '<', &left, &right);
    if (v.len + (left + right) * fill_bytes(s.fill) <= SMALL) {
        cx_u8 buf[SMALL];
        cx_usize k = fill_into(buf, s.fill, left);
        memcpy(buf + k, v.ptr, v.len);
        k += v.len;
        k += fill_into(buf + k, s.fill, right);
        cx_print_str((cx_str){buf, k});
        return;
    }
    put_fill(s.fill, left);
    cx_print_str(v);
    put_fill(s.fill, right);
}

void cx_format_rune(cx_rune v, cx_spec s) {
    cx_u8 b[4];
    int n = utf8_put(b, v);
    cx_format_str((cx_str){b, (cx_usize)n}, s);
}

/* The digits, with `zeros` zeros before them, in groups of `group` with `sep` between. */
static void put_grouped(cx_str digits, cx_usize zeros, char sep, int group) {
    cx_usize total = zeros + digits.len;
    if (!sep) {
        static const cx_u8 z[32] = "00000000000000000000000000000000";
        while (zeros) {
            cx_usize k = zeros < 32 ? zeros : 32;
            cx_print_str((cx_str){z, k});
            zeros -= k;
        }
        cx_print_str(digits);
        return;
    }
    cx_u8 buf[128];
    cx_usize k = 0;
    for (cx_usize i = 0; i < total; i++) {
        if (i > 0 && (total - i) % (cx_usize)group == 0) buf[k++] = (cx_u8)sep;
        buf[k++] = i < zeros ? '0' : digits.ptr[i - zeros];
        if (k >= sizeof buf - 2) {
            cx_print_str((cx_str){buf, k});
            k = 0;
        }
    }
    cx_print_str((cx_str){buf, k});
}

static cx_usize grouped_len(cx_usize n, char sep, int group) { return n + (sep && n ? (n - 1) / (cx_usize)group : 0); }

void cx_format_number(cx_str sign, cx_str prefix, cx_str digits, cx_str rest, cx_spec s, int group) {
    char sep = s.flags & CX_SPEC_COMMA ? ',' : s.flags & CX_SPEC_UNDER ? '_' : 0;
    cx_usize fixed = sign.len + prefix.len + rest.len, zeros = 0;
    if (s.flags & CX_SPEC_ZERO) {
        /* As few zeros as reach the width; a separator can't come first, so a group may overflow
         * it by one, as in Python. */
        while (fixed + grouped_len(digits.len + zeros, sep, group) < s.width) zeros++;
    }
    cx_usize len = fixed + grouped_len(digits.len + zeros, sep, group), left, right;
    split_pad(s, len, '>', &left, &right);
    if (len + (left + right) * fill_bytes(s.fill) <= SMALL) {
        cx_u8 buf[SMALL];
        cx_usize k = fill_into(buf, s.fill, left);
        memcpy(buf + k, sign.ptr, sign.len);
        k += sign.len;
        memcpy(buf + k, prefix.ptr, prefix.len);
        k += prefix.len;
        cx_usize total = zeros + digits.len;
        if (!sep) {
            memset(buf + k, '0', zeros);
            memcpy(buf + k + zeros, digits.ptr, digits.len);
            k += total;
        } else {
            for (cx_usize i = 0; i < total; i++) {
                if (i > 0 && (total - i) % (cx_usize)group == 0) buf[k++] = (cx_u8)sep;
                buf[k++] = i < zeros ? '0' : digits.ptr[i - zeros];
            }
        }
        memcpy(buf + k, rest.ptr, rest.len);
        k += rest.len;
        k += fill_into(buf + k, s.fill, right);
        cx_print_str((cx_str){buf, k});
        return;
    }
    put_fill(s.fill, left);
    cx_print_str(sign);
    cx_print_str(prefix);
    put_grouped(digits, zeros, sep, group);
    cx_print_str(rest);
    put_fill(s.fill, right);
}

static void format_int(cx_u64 v, int negative, cx_spec s) {
    char buf[64];
    int i = 64, base = 10, shift = 0;
    const char* abc = "0123456789abcdef";
    switch (s.kind) {
        case 'x': base = 16, shift = 4; break;
        case 'X': base = 16, shift = 4, abc = "0123456789ABCDEF"; break;
        case 'o': base = 8, shift = 3; break;
        case 'b': base = 2, shift = 1; break;
        default: break;
    }
    if (shift) {
        cx_u64 mask = (cx_u64)base - 1;
        do {
            buf[--i] = abc[v & mask];
            v >>= shift;
        } while (v);
    } else {
        i = 64 - cx_u64_text(v, buf + 64);
        /* The usual padded number, `{n:>8}` or `{n:8}`: the sign and the fill go in front of the
         * digits where they are, and it's all one write, with nothing copied (CLX-188). */
        if (!(s.flags & (CX_SPEC_PLUS | CX_SPEC_ALT | CX_SPEC_ZERO | CX_SPEC_COMMA | CX_SPEC_UNDER)) && s.fill < 0x80 &&
            (s.align == 0 || s.align == '>') && s.width <= 64) {
            if (negative) buf[--i] = '-';
            while (64 - i < (int)s.width) buf[--i] = (char)s.fill;
            cx_print_str((cx_str){(const cx_u8*)buf + i, (cx_usize)(64 - i)});
            return;
        }
    }
    cx_str sign = {(const cx_u8*)"", 0}, prefix = {(const cx_u8*)"", 0};
    if (negative) sign = (cx_str){(const cx_u8*)"-", 1};
    else if (s.flags & CX_SPEC_PLUS) sign = (cx_str){(const cx_u8*)"+", 1};
    if (s.flags & CX_SPEC_ALT) {
        prefix = (cx_str){(const cx_u8*)(base == 16 ? "0x" : base == 8 ? "0o" : "0b"), 2};
    }
    cx_format_number(sign, prefix, (cx_str){(const cx_u8*)buf + i, (cx_usize)(64 - i)}, (cx_str){(const cx_u8*)"", 0}, s,
                     base == 10 ? 3 : 4);
}

void cx_format_i64(cx_i64 v, cx_spec s) { format_int(v < 0 ? (cx_u64)0 - (cx_u64)v : (cx_u64)v, v < 0, s); }

void cx_format_u64(cx_u64 v, cx_spec s) { format_int(v, 0, s); }
