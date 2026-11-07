/* Text methods that build, split and join strings: `trim`, `split`, `lines`, `replace`, `repeat`,
 * `pad_start` and the rest, and a list's `join`. `trim` gives a part of the text, which the
 * emitter copies only where it's kept, as it does a slice. Linked only into programs that call
 * them.
 *
 * Whitespace is Unicode's White_Space, as Rust's `trim` and `split_whitespace` take it: the ASCII
 * tab, line feed, vertical tab, form feed, carriage return and space, and U+0085, U+00A0, U+1680,
 * U+2000 to U+200A, U+2028, U+2029, U+202F, U+205F and U+3000. Python's `strip` takes U+001C to
 * U+001F too. Java's `strip` takes those as well, and keeps U+0085, U+00A0, U+2007 and U+202F. */
#include "calx_rt.h"

#include <string.h>

CX_COLD void cx_panic_text(const char* at, const char* message);

static int ascii_space(cx_u8 b) { return b == ' ' || (b >= 9 && b <= 13); }

static int is_space(cx_rune r) {
    if (r < 0x80) return ascii_space((cx_u8)r);
    return r == 0x85 || r == 0xA0 || r == 0x1680 || (r >= 0x2000 && r <= 0x200A) || r == 0x2028 || r == 0x2029 || r == 0x202F ||
           r == 0x205F || r == 0x3000;
}

/* Whether a space starts at `i`, and where it ends. */
static int space_at(cx_str s, cx_usize i, cx_usize* end) {
    cx_u8 b = s.ptr[i];
    if (b < 0x80) {
        *end = i + 1;
        return ascii_space(b);
    }
    cx_usize j = i;
    cx_rune r = cx_utf8_next(s.ptr, &j);
    *end = j;
    return is_space(r);
}

/* Whether a space ends at `end`, and where it starts. */
static int space_before(cx_str s, cx_usize end, cx_usize* start) {
    cx_u8 b = s.ptr[end - 1];
    if (b < 0x80) {
        *start = end - 1;
        return ascii_space(b);
    }
    cx_usize i = end - 1;
    while (i > 0 && (s.ptr[i] & 0xC0) == 0x80) i--;
    *start = i;
    cx_usize j = i;
    return is_space(cx_utf8_next(s.ptr, &j));
}

/* `s` without the whitespace at its start (`ends` 1), its end (2), or both (3). */
cx_str cx_text_trim(cx_str s, cx_u32 ends) {
    cx_usize lo = 0, hi = s.len, k;
    if (ends & 1) {
        while (lo < hi && space_at(s, lo, &k)) lo = k;
    }
    if (ends & 2) {
        while (hi > lo && space_before(s, hi, &k)) hi = k;
    }
    return (cx_str){s.ptr + lo, hi - lo};
}

static cx_usize add_or_panic(cx_usize a, cx_usize b, const char* at) {
    cx_usize r;
    if (__builtin_add_overflow(a, b, &r)) cx_panic_text(at, "the text would be longer than memory can hold");
    return r;
}

static cx_usize mul_or_panic(cx_usize a, cx_usize b, const char* at) {
    cx_usize r;
    if (__builtin_mul_overflow(a, b, &r)) cx_panic_text(at, "the text would be longer than memory can hold");
    return r;
}

/* `s` with every `from` replaced by `to`, left to right, each match starting after the last one
 * ends. An empty `from` matches before each rune and at the end, as in Java and Rust. */
cx_String cx_text_replace(cx_str s, cx_str from, cx_str to, const char* at) {
    cx_String r = {0, 0, 0};
    if (from.len == 0) {
        cx_usize runes = 0;
        for (cx_usize i = 0; i < s.len; i++) runes += (s.ptr[i] & 0xC0) != 0x80;
        cx_string_reserve(&r, add_or_panic(s.len, mul_or_panic(to.len, runes + 1, at), at));
        cx_string_push_str(&r, to);
        cx_usize i = 0;
        while (i < s.len) {
            cx_usize j = i;
            cx_utf8_next(s.ptr, &j);
            cx_string_push_bytes(&r, s.ptr + i, j - i);
            cx_string_push_str(&r, to);
            i = j;
        }
        return r;
    }
    cx_usize i = 0;
    for (;;) {
        cx_usize k = cx_str_find((cx_str){s.ptr + i, s.len - i}, from);
        if (k == CX_NOT_FOUND) break;
        if (r.cap == 0) cx_string_reserve(&r, s.len + (to.len > from.len ? to.len - from.len : 0) + 16);
        cx_string_push_bytes(&r, s.ptr + i, k);
        cx_string_push_str(&r, to);
        i += k + from.len;
    }
    cx_string_push_bytes(&r, s.ptr + i, s.len - i);
    return r;
}

/* `s` written `n` times. The copies double, so it's a few large copies, not `n` small ones. */
cx_String cx_text_repeat(cx_str s, cx_usize n, const char* at) {
    cx_String r = {0, 0, 0};
    if (n == 0 || s.len == 0) return r;
    cx_usize total = mul_or_panic(s.len, n, at);
    cx_string_reserve(&r, total);
    memcpy(r.ptr, s.ptr, s.len);
    cx_usize len = s.len;
    while (len <= total - len) {
        memcpy(r.ptr + len, r.ptr, len);
        len *= 2;
    }
    memcpy(r.ptr + len, r.ptr, total - len);
    r.len = total;
    return r;
}

/* `s` with `fill` added before it (`start`) or after it until it's `width` runes long. Text that
 * long already comes back as it is. */
cx_String cx_text_pad(cx_str s, cx_usize width, cx_rune fill, cx_bool start, const char* at) {
    cx_usize runes = 0;
    for (cx_usize i = 0; i < s.len; i++) runes += (s.ptr[i] & 0xC0) != 0x80;
    if (runes >= width) return cx_string_from(s);
    cx_String f = {0, 0, 0};
    cx_string_push_rune(&f, fill);
    cx_usize n = width - runes;
    cx_String r = {0, 0, 0};
    cx_string_reserve(&r, add_or_panic(s.len, mul_or_panic(f.len, n, at), at));
    if (!start) cx_string_push_str(&r, s);
    for (cx_usize i = 0; i < n; i++) cx_string_push_bytes(&r, f.ptr, f.len);
    if (start) cx_string_push_str(&r, s);
    cx_drop_String(&f);
    return r;
}

static void list_push(cx_StringList* l, const cx_u8* p, cx_usize n) {
    l->ptr = (cx_String*)cx_grow(l->ptr, &l->cap, l->len, 1, sizeof(cx_String));
    cx_String part = {0, 0, 0};
    cx_string_push_bytes(&part, p, n);
    l->ptr[l->len++] = part;
}

/* The parts of `s` between each `sep`, empty ones too, so there's one more part than there are
 * separators. An empty `sep` gives each rune as a part of its own. */
cx_StringList cx_text_split(cx_str s, cx_str sep) {
    cx_StringList l = {0, 0, 0};
    if (sep.len == 0) {
        cx_usize i = 0;
        while (i < s.len) {
            cx_usize j = i;
            cx_utf8_next(s.ptr, &j);
            list_push(&l, s.ptr + i, j - i);
            i = j;
        }
        return l;
    }
    cx_usize i = 0;
    for (;;) {
        cx_usize k = cx_str_find((cx_str){s.ptr + i, s.len - i}, sep);
        if (k == CX_NOT_FOUND) break;
        list_push(&l, s.ptr + i, k);
        i += k + sep.len;
    }
    list_push(&l, s.ptr + i, s.len - i);
    return l;
}

/* The words of `s`: the parts between runs of whitespace, none of them empty. */
cx_StringList cx_text_split_whitespace(cx_str s) {
    cx_StringList l = {0, 0, 0};
    cx_usize i = 0, k;
    while (i < s.len) {
        while (i < s.len && space_at(s, i, &k)) i = k;
        if (i == s.len) break;
        cx_usize start = i;
        while (i < s.len && !space_at(s, i, &k)) i = k;
        list_push(&l, s.ptr + start, i - start);
    }
    return l;
}

/* The lines of `s`, each without the `\n`, `\r\n` or `\r` that ends it, as `std.io.Lines` reads
 * them. A last line needn't end with one, and an ending at the very end doesn't start another. */
cx_StringList cx_text_lines(cx_str s) {
    cx_StringList l = {0, 0, 0};
    cx_usize i = 0;
    while (i < s.len) {
        cx_usize j = i;
        while (j < s.len && s.ptr[j] != '\n' && s.ptr[j] != '\r') j++;
        list_push(&l, s.ptr + i, j - i);
        if (j < s.len && s.ptr[j] == '\r' && j + 1 < s.len && s.ptr[j + 1] == '\n') j++;
        i = j + 1;
    }
    return l;
}

/* The parts, with `sep` between each two. */
cx_String cx_text_join(const cx_String* parts, cx_usize n, cx_str sep, const char* at) {
    cx_String r = {0, 0, 0};
    if (n == 0) return r;
    cx_usize total = mul_or_panic(sep.len, n - 1, at);
    for (cx_usize i = 0; i < n; i++) total = add_or_panic(total, parts[i].len, at);
    cx_string_reserve(&r, total);
    for (cx_usize i = 0; i < n; i++) {
        if (i) cx_string_push_str(&r, sep);
        cx_string_push_bytes(&r, parts[i].ptr, parts[i].len);
    }
    return r;
}

cx_String cx_text_join_str(const cx_str* parts, cx_usize n, cx_str sep, const char* at) {
    cx_String r = {0, 0, 0};
    if (n == 0) return r;
    cx_usize total = mul_or_panic(sep.len, n - 1, at);
    for (cx_usize i = 0; i < n; i++) total = add_or_panic(total, parts[i].len, at);
    cx_string_reserve(&r, total);
    for (cx_usize i = 0; i < n; i++) {
        if (i) cx_string_push_str(&r, sep);
        cx_string_push_str(&r, parts[i]);
    }
    return r;
}

/* A loop over `s.split_whitespace()`: the next word from `*at`. */
cx_bool cx_text_words_next(cx_str s, cx_usize* at, cx_str* part) {
    cx_usize i = *at, k;
    while (i < s.len && space_at(s, i, &k)) i = k;
    if (i >= s.len) {
        *at = s.len;
        return 0;
    }
    cx_usize start = i;
    while (i < s.len && !space_at(s, i, &k)) i = k;
    *part = (cx_str){s.ptr + start, i - start};
    *at = i;
    return 1;
}
