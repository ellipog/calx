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
    cx_usize rare = cx_bytes_rare(from.ptr, from.len);
    cx_usize i = 0;
    for (;;) {
        cx_usize k = cx_bytes_find_rare(s.ptr + i, s.len - i, from.ptr, from.len, rare);
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

/* Copies `n` bytes, 1 or more, from `p` to `q`. Up to 16 take two moves that overlap, in place of
 * a call to `memcpy`, which costs more than the copy for the short parts a split mostly makes. */
static inline void copy_part(cx_u8* q, const cx_u8* p, cx_usize n) {
    if (n >= 8 && n <= 16) {
        cx_u64 a, b;
        memcpy(&a, p, 8);
        memcpy(&b, p + n - 8, 8);
        memcpy(q, &a, 8);
        memcpy(q + n - 8, &b, 8);
    } else if (n >= 4 && n < 8) {
        cx_u32 a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + n - 4, 4);
        memcpy(q, &a, 4);
        memcpy(q + n - 4, &b, 4);
    } else if (n < 4) {
        q[0] = p[0];
        q[n >> 1] = p[n >> 1];
        q[n - 1] = p[n - 1];
    } else {
        memcpy(q, p, n);
    }
}

/* The parts a split has found so far: the first 16 here, on the stack, and then all of them in a
 * list grown as any is. A line split into a few fields gets one allocation, of the size it needs,
 * where a list grown from empty took two or three, at 4, 8 and 16 (CLX-309). */
#define FEW_PARTS 16
typedef struct {
    cx_String few[FEW_PARTS];
    cx_usize n;
    cx_StringList more;
} parts;

/* Adds the `n` bytes at `p` to `b`. With `view`, the part is a view of them, a `String` that
 * doesn't own its bytes, for a list the compiler saw is only looked at while the text it's from
 * stays as it is (CLX-293). Otherwise it's a copy, in a buffer of its own of 16 bytes at least, as
 * `cx_string_grow_to` would make it, but without a call to grow an empty string first. */
static void parts_push(parts* b, const cx_u8* p, cx_usize n, int view) {
    cx_String part = {0, 0, 0};
    if (view) {
        part.ptr = (cx_u8*)p;
        part.len = n;
        part.cap = CX_VIEW_CAP;
    } else if (n) {
        cx_usize cap = n < 16 ? 16 : n;
        part.ptr = (cx_u8*)cx_alloc(cap);
        copy_part(part.ptr, p, n);
        part.len = n;
        part.cap = cap;
    }
    if (b->n < FEW_PARTS) {
        b->few[b->n++] = part;
        return;
    }
    cx_StringList* l = &b->more;
    if (l->len == 0) {
        l->ptr = (cx_String*)cx_alloc(2 * FEW_PARTS * sizeof(cx_String));
        l->cap = 2 * FEW_PARTS;
        memcpy(l->ptr, b->few, FEW_PARTS * sizeof(cx_String));
        l->len = FEW_PARTS;
    }
    l->ptr = (cx_String*)cx_grow(l->ptr, &l->cap, l->len, 1, sizeof(cx_String));
    l->ptr[l->len++] = part;
}

/* The list of the parts in `b`. */
static cx_StringList parts_list(parts* b) {
    if (b->more.len) return b->more;
    cx_StringList l = {0, 0, 0};
    if (b->n) {
        l.ptr = (cx_String*)cx_alloc(b->n * sizeof(cx_String));
        memcpy(l.ptr, b->few, b->n * sizeof(cx_String));
        l.len = l.cap = b->n;
    }
    return l;
}

/* The parts of `s` between each `sep`, empty ones too, so there's one more part than there are
 * separators. An empty `sep` gives each rune as a part of its own. A one-byte `sep` is looked
 * for as `cx_text_split_next` does, and a longer one's rarest bytes are worked out once, not for
 * each part (CLX-300). With `view`, the parts are views of `s` (`parts_push`). It stops after
 * `limit` parts, each as it would be in the whole list. */
static cx_StringList split_upto(cx_str s, cx_str sep, cx_usize limit, int view) {
    parts b;
    b.n = 0;
    b.more = (cx_StringList){0, 0, 0};
    if (sep.len == 0) {
        cx_usize i = 0;
        for (cx_usize found = 0; i < s.len && found < limit; found++) {
            cx_usize j = i;
            cx_utf8_next(s.ptr, &j);
            parts_push(&b, s.ptr + i, j - i, view);
            i = j;
        }
        return parts_list(&b);
    }
    if (sep.len == 1) {
        const cx_u8* p = s.ptr;
        const cx_u8* end = s.ptr + s.len;
        for (cx_usize found = 1;; found++) {
            const cx_u8* e = cx_find2(p, end, sep.ptr[0], sep.ptr[0]);
            parts_push(&b, p, (cx_usize)(e - p), view);
            if (e == end || found == limit) return parts_list(&b);
            p = e + 1;
        }
    }
    cx_usize rare = cx_bytes_rare(sep.ptr, sep.len);
    cx_usize i = 0;
    for (cx_usize found = 1;; found++) {
        cx_usize k = cx_bytes_find_rare(s.ptr + i, s.len - i, sep.ptr, sep.len, rare);
        if (k == CX_NOT_FOUND) break;
        parts_push(&b, s.ptr + i, k, view);
        if (found == limit) return parts_list(&b);
        i += k + sep.len;
    }
    parts_push(&b, s.ptr + i, s.len - i, view);
    return parts_list(&b);
}

cx_StringList cx_text_split(cx_str s, cx_str sep, int view) { return split_upto(s, sep, (cx_usize)-1, view); }

/* Stopping there, a line split into fields of which a program looks at the first few doesn't
 * look for the ends of the rest (CLX-309). */
cx_StringList cx_text_split_first(cx_str s, cx_str sep, cx_usize n, int view) { return split_upto(s, sep, n, view); }

/* The words of `s`: the parts between runs of whitespace, none of them empty. */
cx_StringList cx_text_split_whitespace(cx_str s, int view) {
    parts b;
    b.n = 0;
    b.more = (cx_StringList){0, 0, 0};
    cx_usize i = 0, k;
    while (i < s.len) {
        while (i < s.len && space_at(s, i, &k)) i = k;
        if (i == s.len) break;
        cx_usize start = i;
        while (i < s.len && !space_at(s, i, &k)) i = k;
        parts_push(&b, s.ptr + start, i - start, view);
    }
    return parts_list(&b);
}

/* The lines of `s`, each without the `\n`, `\r\n` or `\r` that ends it, as `std.io.Lines` reads
 * them. A last line needn't end with one, and an ending at the very end doesn't start another. */
cx_StringList cx_text_lines(cx_str s, int view) {
    parts b;
    b.n = 0;
    b.more = (cx_StringList){0, 0, 0};
    cx_usize i = 0;
    while (i < s.len) {
        cx_usize j = (cx_usize)(cx_find2(s.ptr + i, s.ptr + s.len, '\n', '\r') - s.ptr);
        parts_push(&b, s.ptr + i, j - i, view);
        if (j < s.len && s.ptr[j] == '\r' && j + 1 < s.len && s.ptr[j + 1] == '\n') j++;
        i = j + 1;
    }
    return parts_list(&b);
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
