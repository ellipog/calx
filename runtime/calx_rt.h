/* The Calx runtime, as generated code sees it.
 *
 * Every C file the compiler emits includes this header and nothing else, so it includes no
 * system headers: the types come from clang's predefined macros. The helpers here are the parts
 * of Java's semantics that C leaves undefined (CALX_SPEC.md, "Integer and float semantics"). */
#ifndef CALX_RT_H
#define CALX_RT_H

typedef __INT8_TYPE__ cx_i8;
typedef __INT16_TYPE__ cx_i16;
typedef __INT32_TYPE__ cx_i32;
typedef __INT64_TYPE__ cx_i64;
typedef __UINT8_TYPE__ cx_u8;
typedef __UINT16_TYPE__ cx_u16;
typedef __UINT32_TYPE__ cx_u32;
typedef __UINT64_TYPE__ cx_u64;
typedef __SIZE_TYPE__ cx_usize;
typedef _Bool cx_bool;
typedef cx_u16 cx_char;
typedef cx_u32 cx_rune;
typedef float cx_f32;
typedef double cx_f64;

typedef struct {
    const cx_u8* ptr;
    cx_usize len;
} cx_str;

#define CX_INLINE static inline __attribute__((always_inline))
#define CX_COLD __attribute__((cold, noreturn))

/* Each function starts with this for every view parameter: a view points at a whole, live,
 * aligned value for the whole call, since views can't be stored or outlive what they view (D48).
 * Saying so lets clang read a view's fields once before a loop instead of on every pass, even
 * after the function is inlined, which a parameter attribute wouldn't survive. */
#if __has_builtin(__builtin_assume_dereferenceable)
#define CX_VIEW(p) (__builtin_assume_dereferenceable((p), sizeof(*(p))), \
                    (void)__builtin_assume_aligned((p), _Alignof(__typeof__(*(p)))))
#else
#define CX_VIEW(p) ((void)__builtin_assume_aligned((p), _Alignof(__typeof__(*(p)))))
#endif

/* A C function's name as the linker sees it, for `extern "C"` declarations: an asm label gives
 * it, so the name Calx calls it by can't meet one a header declares. */
#define CX_SYMBOL_2(prefix, name) #prefix name
#define CX_SYMBOL_1(prefix, name) CX_SYMBOL_2(prefix, name)
#define CX_SYMBOL(name) CX_SYMBOL_1(__USER_LABEL_PREFIX__, name)

/* `String`: UTF-8 bytes. With `cap` 0 it doesn't own them: an empty string, or a string literal,
 * which costs nothing to make. Changing such a string copies it to the heap first. All zero bits
 * is the empty string, so a value that was moved away can be zeroed and dropped again safely. */
typedef struct {
    cx_u8* ptr;
    cx_usize len;
    cx_usize cap;
} cx_String;

/* Startup and exit, called by the generated `main`. */
void cx_start(int argc, char** argv);
int cx_finish(void);

/* Output. Standard output is buffered, and flushed at exit and before a panic. */
void cx_print_str(cx_str s);
void cx_print_i64(cx_i64 v);
void cx_print_u64(cx_u64 v);
void cx_print_f64(cx_f64 v);
void cx_print_f32(cx_f32 v);
void cx_print_bool(cx_bool v);
void cx_print_char(cx_char v);
void cx_print_rune(cx_rune v);
void cx_print_newline(void);

/* `eprint` and `eprintln`: between these, the print functions write to standard error, which
 * isn't buffered. Output waiting for standard output goes first, so the two keep their order.
 * Afterwards they write where they were writing, which matters inside a `show` written by hand
 * (D102): one that calls `eprint` while its value goes into a string. */
typedef struct {
    void* fmt;
    int err;
} cx_out_to;
cx_out_to cx_err_begin(void);
void cx_err_end(cx_out_to was);

/* A panic: `cx_panic_begin` starts the message on standard error, the print functions add to
 * it, and `cx_panic_end` finishes it and exits with status 101. */
void cx_panic_begin(const char* at);
CX_COLD void cx_panic_end(void);
CX_COLD void cx_panic_index(cx_usize index, cx_usize length, const char* at);
CX_COLD void cx_panic_div_zero(const char* at);
CX_COLD void cx_panic_unwrap(const char* at);
CX_COLD void cx_panic_unwrap_err(const char* at);
CX_COLD void cx_panic_insert(cx_usize index, cx_usize length, const char* at);
CX_COLD void cx_panic_set_len(cx_usize length, cx_usize capacity, const char* at);

/* `parallel for`: calls `body(ctx, begin, end)` for chunks covering 0..n, on the worker threads
 * and this one, and returns once every iteration has run. Inside another `parallel for`, it
 * runs them all on this thread. */
void cx_parallel(cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx);

/* Memory, in calx_alloc.c (D95). A block is freed, or reallocated, with the size it was last
 * given, which the compiler always knows, so blocks have no header. Freeing a null pointer does
 * nothing. Running out panics. */
void* cx_alloc(cx_usize size);
void* cx_realloc(void* p, cx_usize old, cx_usize size);
void cx_free(void* p, cx_usize size);
/* How many blocks are allocated, in debug builds; 0 in others. */
cx_usize cx_alloc_live(void);
/* Makes room for `extra` more elements of `size` bytes in a list's buffer of `*cap`, holding
 * `len`, growing it to at least twice its size. Returns the buffer, which may have moved. The
 * check is inline, so with `size` a constant it has no division. */
void* cx_grow_to(void* ptr, cx_usize* cap, cx_usize need, cx_usize max, cx_usize size);
CX_COLD void cx_panic_capacity(void);
CX_INLINE void* cx_grow(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    /* No buffer can be bigger than half the address space. */
    cx_usize max = ((cx_usize)-1 / 2) / (size ? size : 1);
    if (__builtin_expect(extra > max - len, 0)) cx_panic_capacity();
    cx_usize need = len + extra;
    if (need <= *cap) return ptr;
    return cx_grow_to(ptr, cap, need, max, size);
}
/* Room for `extra` more in a deque's ring, which runs from `*head` round its buffer: grown as a
   list's is, then with the run that wrapped moved so the ring is whole again (D110). */
void* cx_deque_grow(void* ptr, cx_usize* head, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size);

/* Arenas (D114). An arena hands out memory from the bottom of its newest chunk up, and frees it
 * all at once. The state is on the heap, so an `Arena` is one pointer, and the lists made from it
 * hold that pointer, never the `Arena` itself. The first chunk is made when it's first needed. */
typedef struct cx_arena_chunk cx_arena_chunk;
struct cx_arena_chunk {
    cx_arena_chunk* prev; /* the chunk before it, which is full */
    cx_usize size;        /* its size in bytes, this header included */
};
typedef struct cx_arena_state {
    cx_u8* top;            /* the first free byte in the newest chunk, or null before the first */
    cx_u8* end;            /* the end of the newest chunk */
    cx_arena_chunk* chunk; /* the newest chunk */
    cx_usize first;        /* the size of the first chunk */
    cx_usize full;         /* the bytes taken from the chunks before the newest */
} cx_arena_state;
typedef struct cx_Arena {
    cx_arena_state* s;
} cx_Arena;
/* An arena whose first chunk holds at least `first` bytes, 8 KiB if it's 0. */
cx_Arena cx_arena_new(cx_usize first);
void cx_drop_Arena(cx_Arena* a);
/* A new, empty arena with the same first chunk: nothing in an arena belongs to its value. */
cx_Arena cx_copy_Arena(const cx_Arena* a);
/* Frees everything taken from the arena. One that needed more than one chunk keeps a single
 * chunk as big as all of them, so the next round of the same work takes no new chunk. */
void cx_arena_reset(cx_Arena* a);
/* The bytes taken since the arena was made or reset, alignment included. */
cx_usize cx_arena_used(const cx_Arena* a);
/* Moves an arena list's buffer of `len` elements of `size` bytes to `bytes` from a new chunk. */
void* cx_arena_more(cx_arena_state* s, void* ptr, cx_usize len, cx_usize bytes, cx_usize size, cx_usize align);
/* Room for `extra` more elements of `size` bytes, aligned to `align`, in an arena list's buffer
 * of `*cap` holding `len`: grown in place if it's the newest block and its chunk has room, or
 * moved to new memory at least twice its size, and at least 64 bytes. Returns the buffer. All
 * but a new chunk is inline, so with `size` and `align` constants it's a few instructions. */
CX_INLINE void* cx_arena_grow(cx_arena_state* s, void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size,
                              cx_usize align) {
    cx_usize unit = size ? size : 1;
    cx_usize max = ((cx_usize)-1 / 4) / unit;
    if (__builtin_expect(extra > max - len, 0)) cx_panic_capacity();
    cx_usize need = len + extra;
    if (need <= *cap) return ptr;
    cx_usize n = *cap <= max / 2 ? *cap * 2 : max;
    if (n < need) n = need;
    if (n < 64 / unit) n = 64 / unit;
    if (n < 4) n = 4;
    cx_u8* p = (cx_u8*)ptr;
    cx_usize old = *cap * size, bytes = n * size;
    *cap = n;
    /* The newest block grows where it is, while its chunk has room. */
    if (p && p + old == s->top && bytes - old <= (cx_usize)(s->end - s->top)) {
        s->top = p + bytes;
        return p;
    }
    if (s->top) {
        cx_usize pad = (cx_usize)(-(__UINTPTR_TYPE__)s->top & (align - 1));
        cx_usize room = (cx_usize)(s->end - s->top);
        if (pad <= room && bytes <= room - pad) {
            cx_u8* q = s->top + pad;
            s->top = q + bytes;
            if (len) __builtin_memcpy(q, p, len * size);
            return q;
        }
    }
    return cx_arena_more(s, p, len, bytes, size, align);
}
/* `sort` found that a hand-written `Order` contradicts itself (D102). */
CX_COLD void cx_panic_order(const char* at);

/* Strings. */
CX_INLINE cx_String cx_string_lit(const char* p, cx_usize len) {
    cx_String s = {(cx_u8*)p, len, 0};
    return s;
}
CX_INLINE cx_str cx_sv(cx_String s) {
    cx_str v = {s.ptr, s.len};
    return v;
}
/* -1, 0 or 1, as `a` comes before, is the same as, or comes after `b`, byte by byte. For UTF-8
 * that's the order of the code points. */
CX_INLINE int cx_str_cmp(cx_str a, cx_str b) {
    cx_usize n = a.len < b.len ? a.len : b.len;
    if (n >= 8) {
        // Most strings differ in their first 8 bytes, which compare as one big-endian integer.
        cx_u64 x, y;
        __builtin_memcpy(&x, a.ptr, 8);
        __builtin_memcpy(&y, b.ptr, 8);
        if (x != y) return __builtin_bswap64(x) < __builtin_bswap64(y) ? -1 : 1;
        int c = __builtin_memcmp(a.ptr + 8, b.ptr + 8, n - 8);
        if (c) return c < 0 ? -1 : 1;
    } else {
        for (cx_usize i = 0; i < n; i++) {
            if (a.ptr[i] != b.ptr[i]) return a.ptr[i] < b.ptr[i] ? -1 : 1;
        }
    }
    return (a.len > b.len) - (a.len < b.len);
}
/* `xs.sort()` on a list of integers, `char`s, `rune`s or `bool`s, from calx_sort.h (D96). */
void cx_sort_i8(cx_i8* v, cx_usize n);
void cx_sort_i16(cx_i16* v, cx_usize n);
void cx_sort_i32(cx_i32* v, cx_usize n);
void cx_sort_i64(cx_i64* v, cx_usize n);
void cx_sort_u8(cx_u8* v, cx_usize n);
void cx_sort_u16(cx_u16* v, cx_usize n);
void cx_sort_u32(cx_u32* v, cx_usize n);
void cx_sort_u64(cx_u64* v, cx_usize n);
void cx_sort_usize(cx_usize* v, cx_usize n);
void cx_sort_char(cx_char* v, cx_usize n);
void cx_sort_rune(cx_rune* v, cx_usize n);
void cx_sort_bool(cx_bool* v, cx_usize n);
CX_INLINE void cx_drop_String(cx_String* s) {
    if (s->cap) cx_free(s->ptr, s->cap);
}
cx_String cx_copy_String(const cx_String* s);
/* A `String` that owns a copy of the bytes. */
cx_String cx_string_from(cx_str s);
void cx_string_push_bytes(cx_String* s, const cx_u8* p, cx_usize n);
void cx_string_push_rune(cx_String* s, cx_rune r);
void cx_string_reserve(cx_String* s, cx_usize extra);
CX_INLINE void cx_string_push_str(cx_String* s, cx_str v) { cx_string_push_bytes(s, v.ptr, v.len); }
CX_INLINE void cx_string_clear(cx_String* s) {
    if (s->cap) {
        s->len = 0;
    } else {
        s->ptr = 0;
        s->len = 0;
    }
}
cx_bool cx_str_contains(cx_str hay, cx_str needle);
/* Whether the bytes are UTF-8: no overlong forms, surrogates, or values past U+10FFFF. */
cx_bool cx_utf8_valid(const cx_u8* p, cx_usize n);
/* Java's String.hashCode and length(), over the UTF-16 units the text stands for. */
cx_i32 cx_str_java_hash(cx_str s);
cx_usize cx_str_utf16_len(cx_str s);
/* The rune at `*i` in UTF-8 that's known to be valid, moving `*i` past it. */
CX_INLINE cx_rune cx_utf8_next(const cx_u8* p, cx_usize* i) {
    cx_u32 b = p[*i];
    if (b < 0x80) {
        *i += 1;
        return b;
    }
    if (b < 0xE0) {
        cx_rune r = ((b & 0x1F) << 6) | (p[*i + 1] & 0x3F);
        *i += 2;
        return r;
    }
    if (b < 0xF0) {
        cx_rune r = ((b & 0x0F) << 12) | ((cx_u32)(p[*i + 1] & 0x3F) << 6) | (p[*i + 2] & 0x3F);
        *i += 3;
        return r;
    }
    cx_rune r = ((b & 0x07) << 18) | ((cx_u32)(p[*i + 1] & 0x3F) << 12) | ((cx_u32)(p[*i + 2] & 0x3F) << 6) | (p[*i + 3] & 0x3F);
    *i += 4;
    return r;
}
/* The UTF-16 unit at `*i`, as Java sees the text. A rune past U+FFFF gives its high surrogate,
 * and leaves the low one in `*low` for the next call. */
CX_INLINE cx_char cx_utf16_next(const cx_u8* p, cx_usize* i, cx_char* low) {
    if (*low) {
        cx_char c = *low;
        *low = 0;
        return c;
    }
    cx_rune r = cx_utf8_next(p, i);
    if (r < 0x10000) return (cx_char)r;
    r -= 0x10000;
    *low = (cx_char)(0xDC00 | (r & 0x3FF));
    return (cx_char)(0xD800 | (r >> 10));
}

/* `T.parse(text)`. Integers follow Java's Integer.parseInt: an optional sign (no `-` for unsigned
 * types), then ASCII digits, in range. Floats follow Double.parseDouble's grammar, and round
 * correctly. On failure they set `*out` to 0 and return 0. */
cx_bool cx_parse_i64(cx_str s, cx_i64 lo, cx_i64 hi, cx_i64* out);
cx_bool cx_parse_u64(cx_str s, cx_u64 hi, cx_u64* out);
cx_bool cx_parse_f64(cx_str s, cx_f64* out);
cx_bool cx_parse_f32(cx_str s, cx_f32* out);
CX_INLINE cx_bool cx_str_starts_with(cx_str s, cx_str p) {
    return p.len <= s.len && (p.len == 0 || __builtin_memcmp(s.ptr, p.ptr, p.len) == 0);
}
CX_INLINE cx_bool cx_str_ends_with(cx_str s, cx_str p) {
    return p.len <= s.len && (p.len == 0 || __builtin_memcmp(s.ptr + s.len - p.len, p.ptr, p.len) == 0);
}

/* Interpolation into a `String`: between `cx_fmt_begin` and `cx_fmt_end`, the print functions
 * add to the string instead of writing output. They nest. */
cx_String* cx_fmt_begin(cx_String* s);
void cx_fmt_end(cx_String* previous);

/* Whether two strings hold the same bytes, for string patterns. */
CX_INLINE cx_bool cx_str_eq(cx_str a, cx_str b) {
    return a.len == b.len && (a.len == 0 || __builtin_memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* `s[lo..hi]`, or `s[lo..=hi]` with `inclusive`: the bytes from `lo` up to `hi`, which must be in
 * order, in the text, and where runes start (D117). */
CX_COLD void cx_panic_slice(cx_str s, cx_usize lo, cx_usize hi, cx_bool inclusive, const char* at);
CX_INLINE cx_bool cx_utf8_starts_at(cx_str s, cx_usize k) { return k == s.len || (s.ptr[k] & 0xC0) != 0x80; }
CX_INLINE cx_str cx_str_slice(cx_str s, cx_usize lo, cx_usize hi, cx_bool inclusive, const char* at) {
    cx_usize end = hi + (inclusive ? 1 : 0);
    if (__builtin_expect(lo > end || end > s.len || end < hi || !cx_utf8_starts_at(s, lo) || !cx_utf8_starts_at(s, end), 0)) {
        cx_panic_slice(s, lo, hi, inclusive, at);
    }
    return (cx_str){s.ptr + lo, end - lo};
}
/* Where `needle` first or last starts in `hay`, a byte offset, or CX_NOT_FOUND. Both are UTF-8,
 * so a match starts where a rune does. `""` is found at 0, or last at the end. */
cx_usize cx_str_find(cx_str hay, cx_str needle);
cx_usize cx_str_find_last(cx_str hay, cx_str needle);
/* Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)`, in calx_case.c (D117). Text
 * that doesn't change comes back without a copy when `keep` says its bytes are a literal's. */
cx_String cx_case_lower(cx_str s, cx_bool keep);
cx_String cx_case_upper(cx_str s, cx_bool keep);

/* MD5, SHA-1 and SHA-256 (kind 0, 1 and 2) and HMAC-SHA256, in calx_digest.c (D118). A context is
 * CX_DIGEST_CTX bytes, with no alignment needed. `cx_digest_finish` writes the digest, 16, 20 or
 * 32 bytes, and returns its length. */
#define CX_DIGEST_CTX 112
void cx_digest_start(cx_u8* ctx, cx_u32 kind);
void cx_digest_add(cx_u8* ctx, const cx_u8* p, cx_usize n);
cx_usize cx_digest_finish(cx_u8* ctx, cx_u8* out);
void cx_digest_hmac_sha256(const cx_u8* key, cx_usize key_len, const cx_u8* msg, cx_usize msg_len, cx_u8* out);

/* Random bytes from the system, AES-128-CFB8 and RSA, in calx_crypto.c (D118). The contexts are
 * CX_CFB8_CTX and CX_RSA_CTX bytes, with no alignment needed. */
#define CX_CFB8_CTX 208
#define CX_RSA_CTX 912
void cx_crypto_random(cx_u8* p, cx_usize n);
void cx_cfb8_start(cx_u8* ctx, const cx_u8* key, const cx_u8* iv);
void cx_cfb8_encrypt(cx_u8* ctx, cx_u8* p, cx_usize n);
void cx_cfb8_decrypt(cx_u8* ctx, cx_u8* p, cx_usize n);
cx_bool cx_rsa_generate(cx_u8* ctx, cx_u32 bits);
cx_usize cx_rsa_public_der(const cx_u8* ctx, cx_u8* out);
cx_bool cx_rsa_encrypt(const cx_u8* ctx, const cx_u8* msg, cx_usize len, cx_u8* out);
cx_i64 cx_rsa_decrypt(const cx_u8* ctx, const cx_u8* in, cx_usize len, cx_u8* out);
cx_usize cx_rsa_size(const cx_u8* ctx);

/* Hashing, for `Map` and `Set` (D94). The same value hashes the same on every platform and in
 * every run, so a table's layout, and the order a loop goes over it, depend only on what was
 * done to it. A word is mixed in with a folded multiply: the 128-bit product's halves, xored. */
#define CX_HASH_SEED 0x243F6A8885A308D3ull
#define CX_HASH_MUL 0x9E3779B97F4A7C15ull
CX_INLINE cx_u64 cx_hash_mix(cx_u64 a, cx_u64 b) {
    __uint128_t r = (__uint128_t)a * b;
    return (cx_u64)r ^ (cx_u64)(r >> 64);
}
CX_INLINE cx_u64 cx_hash_word(cx_u64 h, cx_u64 w) { return cx_hash_mix(h ^ w, CX_HASH_MUL); }
cx_u64 cx_hash_bytes(const cx_u8* p, cx_usize n);
CX_INLINE cx_u64 cx_hash_str(cx_str s) { return cx_hash_bytes(s.ptr, s.len); }

/* The tables behind `Map` and `Set`: open addressing, with a control byte per slot that's EMPTY,
 * DELETED, or the top 7 bits of the hash of what's there. Lookups compare 16 control bytes at
 * once, and the 16 after the last slot repeat the first 16, so a group never wraps. A table has
 * a power of two slots, at least 4, filled to at most 7/8. An empty table has no slots, and its
 * control bytes are `cx_table_empty`'s, which every lookup stops at. */
#define CX_CTRL_EMPTY ((cx_u8)0x80)
#define CX_CTRL_DELETED ((cx_u8)0xFE)
#define CX_GROUP 16
#define CX_NOT_FOUND ((cx_usize)-1)
extern const cx_u8 cx_table_empty[CX_GROUP];

#if defined(__SSE2__)
typedef char cx_group_v __attribute__((vector_size(16), aligned(1)));
CX_INLINE cx_u32 cx_group_match(const cx_u8* g, cx_u8 b) {
    cx_group_v v = *(const cx_group_v*)g;
    return (cx_u32)__builtin_ia32_pmovmskb128(v == (char)b);
}
/* EMPTY or DELETED: the control bytes with the top bit set. */
CX_INLINE cx_u32 cx_group_free(const cx_u8* g) {
    return (cx_u32)__builtin_ia32_pmovmskb128(*(const cx_group_v*)g);
}
#else
CX_INLINE cx_u32 cx_group_match(const cx_u8* g, cx_u8 b) {
    cx_u32 m = 0;
    for (int i = 0; i < CX_GROUP; i++) m |= (cx_u32)(g[i] == b) << i;
    return m;
}
CX_INLINE cx_u32 cx_group_free(const cx_u8* g) {
    cx_u32 m = 0;
    for (int i = 0; i < CX_GROUP; i++) m |= (cx_u32)(g[i] >> 7) << i;
    return m;
}
#endif
CX_INLINE cx_u32 cx_group_empty(const cx_u8* g) { return cx_group_match(g, CX_CTRL_EMPTY); }
/* The slots that hold a value. A loop over a table goes a group at a time: from 0 in steps of
 * 16 while the step is at most the mask, then over these bits. A table smaller than a group has
 * EMPTY bytes after its slots, so only its own show. */
CX_INLINE cx_u32 cx_group_full(const cx_u8* g) { return ~cx_group_free(g) & 0xFFFFu; }
CX_INLINE cx_u8 cx_table_tag(cx_u64 h) { return (cx_u8)(h >> 57); }
/* How many values fit in a table of `buckets` slots. */
CX_INLINE cx_usize cx_table_capacity(cx_usize buckets) { return buckets < 8 ? buckets - 1 : buckets / 8 * 7; }
/* Sets slot `i`'s control byte, and its copy past the end if it has one. */
CX_INLINE void cx_table_set(cx_u8* ctrl, cx_usize mask, cx_usize i, cx_u8 c) {
    ctrl[i] = c;
    ctrl[((i - CX_GROUP) & mask) + CX_GROUP] = c;
}
/* The first EMPTY or DELETED slot where the hash `h` would go. A group can show a slot past the
 * end of a table smaller than a group, which wraps to a full one; then the first group has one. */
CX_INLINE cx_usize cx_table_free_slot(const cx_u8* ctrl, cx_usize mask, cx_u64 h) {
    cx_usize pos = (cx_usize)h & mask, stride = 0;
    for (;;) {
        cx_u32 bits = cx_group_free(ctrl + pos);
        if (bits) {
            cx_usize i = (pos + (cx_usize)__builtin_ctz(bits)) & mask;
            if (__builtin_expect(ctrl[i] < 0x80, 0)) i = (cx_usize)__builtin_ctz(cx_group_free(ctrl));
            return i;
        }
        stride += CX_GROUP;
        pos = (pos + stride) & mask;
    }
}
/* Marks slot `i` free after its value is taken out. It can become EMPTY, which ends lookups, only
 * if no lookup could have passed it: if a group around it already has an EMPTY. */
CX_INLINE cx_bool cx_table_erase(cx_u8* ctrl, cx_usize mask, cx_usize i) {
    cx_u32 before = cx_group_empty(ctrl + ((i - CX_GROUP) & mask));
    cx_u32 after = cx_group_empty(ctrl + i);
    /* The EMPTY bytes just before slot `i` and from it, counted without a branch: 16 when the
     * group has none. clang 19 counts a 16-bit value directly; before it, an extra bit stops the
     * count at 16. */
#if __has_builtin(__builtin_clzg)
    int lead = __builtin_clzg((cx_u16)before, 16);
    int trail = __builtin_ctzg((cx_u16)after, 16);
#else
    int lead = __builtin_clz((before << 16) | 0x8000u);
    int trail = __builtin_ctz(after | 0x10000u);
#endif
    cx_bool empty = lead + trail < CX_GROUP;
    cx_table_set(ctrl, mask, i, empty ? CX_CTRL_EMPTY : CX_CTRL_DELETED);
    return empty;
}
/* The slot count to grow to when a value needs room: double, or the same count again if half
 * the room went to DELETED slots, which rebuilding frees. */
CX_INLINE cx_usize cx_table_grown(cx_usize buckets, cx_usize len) {
    if (buckets == 0) return 4;
    return len + 1 <= cx_table_capacity(buckets) / 2 ? buckets : buckets * 2;
}
/* The slot count with room for `n` values. */
cx_usize cx_table_buckets_for(cx_usize n);
/* A table of `buckets` slots of `size` bytes, all EMPTY: gives the control bytes, and the slots
 * through `slots`. One allocation holds both, so freeing the slots frees the table. */
cx_u8* cx_table_alloc(cx_usize buckets, cx_usize size, void** slots);
/* Frees what cx_table_alloc gave for `buckets` slots of `size` bytes. */
void cx_table_free(void* slots, cx_usize buckets, cx_usize size);

/* A `LinkedMap` or a `LinkedSet` is a table as above with a list through its full slots, in the
 * order their keys went in (D109). `link[2 * i]` is the slot before slot `i` and `link[2 * i + 1]`
 * the one after, so both are in the same cache line; `first` and `last` are the ends. Each is the
 * slot plus 1, and 0 for none, so a table that's all zeros is an empty list. Only full slots'
 * links are ever read. */
typedef struct cx_lhead {
    cx_u32* link;
    cx_u32 first, last;
} cx_lhead;
CX_INLINE void cx_lk_unlink(cx_lhead* l, cx_usize i) {
    cx_u32 prev = l->link[2 * i], next = l->link[2 * i + 1];
    if (prev) l->link[2 * (prev - 1) + 1] = next;
    else l->first = next;
    if (next) l->link[2 * (next - 1)] = prev;
    else l->last = prev;
}
CX_INLINE void cx_lk_push_back(cx_lhead* l, cx_usize i) {
    cx_u32 at = (cx_u32)i + 1;
    l->link[2 * i] = l->last;
    l->link[2 * i + 1] = 0;
    if (l->last) l->link[2 * (l->last - 1) + 1] = at;
    else l->first = at;
    l->last = at;
}
CX_INLINE void cx_lk_push_front(cx_lhead* l, cx_usize i) {
    cx_u32 at = (cx_u32)i + 1;
    l->link[2 * i] = 0;
    l->link[2 * i + 1] = l->first;
    if (l->first) l->link[2 * (l->first - 1)] = at;
    else l->last = at;
    l->first = at;
}
/* Moves slot `i`, which is in the list, to its front or its back. */
CX_INLINE void cx_lk_move(cx_lhead* l, cx_usize i, cx_bool front) {
    cx_u32 at = (cx_u32)i + 1;
    if ((front ? l->first : l->last) == at) return;
    cx_lk_unlink(l, i);
    if (front) cx_lk_push_front(l, i);
    else cx_lk_push_back(l, i);
}
/* The links for a table of `buckets` slots, which can't have more than a `cx_u32` counts. */
cx_u32* cx_lk_alloc(cx_usize buckets);
void cx_lk_free(cx_u32* link, cx_usize buckets);
CX_COLD void cx_panic_key(const char* at);
/* `m.range(a..b)` on a `SortedMap` or a `SortedSet`, with `b` before `a` (D112). */
CX_COLD void cx_panic_range(const char* at);
/* `q.remove_marked(marks)` on a `PriorityQueue`, without one mark for each element (D113). */
CX_COLD void cx_panic_marks(cx_usize marks, cx_usize length, const char* at);
/* An `OpenHashMap`'s or an `OpenHashSet`'s load factor isn't between 0 and 1, or it's asked for
 * more keys than fastutil's tables hold. */
CX_COLD void cx_panic_load_factor(const char* at);
CX_COLD void cx_panic_table_size(const char* at);

/* The tables behind `JavaHashMap` and `JavaHashSet`, in calx_jtable.c (D105): Java's HashMap,
 * which iterates in Java's order. `heads` is a power of two bins, each 0 when empty, the first
 * node plus one for a list, and minus that for a red-black tree. The nodes are in one array, each
 * starting with its key's spread hash and the next node in its bin, or -1; the compiler's node
 * type follows them with the key, or the key and value. A node that's removed waits in `spare`
 * for the next key. Tree bins keep their links in `tree`, beside the nodes, made with the first. */
typedef struct {
    cx_i32 prev, parent, left, right;
    cx_bool red;
} cx_jtnode;
typedef struct {
    cx_i32* heads;    /* `cap` bins, or null before the first key */
    void* nodes;      /* room for `room` nodes, `used` of them handed out */
    cx_jtnode* tree;  /* the tree links, for `room` nodes, or null */
    cx_usize len;     /* the keys */
    cx_u32 cap;       /* the bins */
    cx_u32 thr;       /* Java's threshold: the table doubles when `len` passes it. Before the first
                       * table, the bins Java's `new HashMap<>(n)` asked for, or 0 for 16 */
    cx_u32 used;
    cx_u32 room;
    cx_u32 spare;     /* a removed node plus one, or 0; its `next` is the one removed before it */
} cx_jtable;
/* Whether node `a`'s key orders before (-1), with (0) or after (1) node `b`'s, for tree bins. */
typedef int (*cx_jt_order)(const void* nodes, cx_i32 a, cx_i32 b);
#define CX_JT_HASH(nodes, size, i) (((cx_i32*)((char*)(nodes) + (cx_usize)(i) * (size)))[0])
#define CX_JT_NEXT(nodes, size, i) (((cx_i32*)((char*)(nodes) + (cx_usize)(i) * (size)))[1])
/* Java spreads a key's `hashCode` so its top bits pick bins in small tables too. */
CX_INLINE cx_i32 cx_jt_spread(cx_i32 h) { return h ^ (cx_i32)((cx_u32)h >> 16); }
/* The first node of a bin, or -1. */
CX_INLINE cx_i32 cx_jt_first(cx_i32 head) { return (head < 0 ? -head : head) - 1; }
CX_INLINE cx_i32 cx_java_long_hash(cx_u64 v) { return (cx_i32)(v ^ (v >> 32)); }
/* `Float.hashCode` and `Double.hashCode`: the bits, with every NaN as Java's one NaN. */
CX_INLINE cx_i32 cx_java_float_hash(cx_f32 f) {
    cx_u32 b = 0x7FC00000u;
    if (f == f) __builtin_memcpy(&b, &f, 4);
    return (cx_i32)b;
}
CX_INLINE cx_i32 cx_java_double_hash(cx_f64 d) {
    cx_u64 b = 0x7FF8000000000000ull;
    if (d == d) __builtin_memcpy(&b, &d, 8);
    return (cx_i32)(b ^ (b >> 32));
}
/* The bins Java's `new HashMap<>(n)` starts with: the power of two at least `n`. */
cx_u32 cx_jt_size_for(cx_usize n);
/* A node for a new key, with its hash and `next` still to set. The nodes can move. */
cx_i32 cx_jt_new_node(cx_jtable* m, cx_usize size);
CX_INLINE void cx_jt_release(cx_jtable* m, cx_usize size, cx_i32 i) {
    CX_JT_NEXT(m->nodes, size, i) = (cx_i32)m->spare - 1;
    m->spare = (cx_u32)i + 1;
}
/* Java's resize: the first table, or twice the bins, each bin split in order. */
void cx_jt_resize(cx_jtable* m, cx_usize size, cx_jt_order order);
/* A list bin that a key made too long: a tree, or a table twice the size if it's small. */
void cx_jt_treeify_bin(cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32 h);
/* Adds the node `x`, with its hash and key set, to the tree in bin `b`. */
void cx_jt_tree_put(cx_jtable* m, cx_usize size, cx_jt_order order, cx_u32 b, cx_i32 x);
/* Takes the node `z` out of the tree in bin `b`, which may become a list. */
void cx_jt_tree_remove(cx_jtable* m, cx_usize size, cx_u32 b, cx_i32 z);
void cx_jt_reserve(cx_jtable* m, cx_usize size, cx_jt_order order, cx_usize n);
void cx_jt_free(cx_jtable* m, cx_usize size);
cx_jtable cx_jt_copy(const cx_jtable* m, cx_usize size);
void cx_jt_clear(cx_jtable* m);
/* Java's `String.compareTo`, by UTF-16 unit: -1, 0 or 1. */
int cx_jt_str_order(cx_str a, cx_str b);

/* The tables behind `OpenHashMap` and `OpenHashSet` (D107): fastutil's open hash tables, which
 * iterate in fastutil's order. A table is `n` slots, a power of two, found by linear probing from
 * the slot its key's mixed hash picks. An integer key of 0 marks an empty slot, so the key 0
 * itself is kept in slot `n`, past the others, and `zero` says whether it's there. Other keys
 * have a hash word per slot, 0 when it's empty. The compiler's table type starts with its slots
 * and those words, then this header; finding, adding, removing and rehashing are the compiler's,
 * for each key type, and the sizes fastutil picks are here, in calx_ohtable.c. */
typedef struct {
    cx_u32 n;         /* the slots, or 0 in a table made without a size, before its first use */
    cx_u32 mask;      /* n - 1 */
    cx_u32 len;       /* the keys */
    cx_u32 max_fill;  /* an insert that finds `len` at this rehashes */
    cx_u32 min_n;     /* the slots it was made with, which removing keys won't shrink it below */
    cx_f32 f;         /* the load factor */
    cx_bool zero;     /* integer keys: whether 0 is a key */
} cx_ohead;
/* `HashCommon.mix`: the hash an `int`, a `long` or an object's `hashCode` places a key by. */
CX_INLINE cx_u32 cx_oh_mix32(cx_i32 x) {
    cx_u32 h = (cx_u32)x * 0x9E3779B9u;
    return h ^ (h >> 16);
}
CX_INLINE cx_u32 cx_oh_mix64(cx_u64 x) {
    cx_u64 h = x * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return (cx_u32)(h ^ (h >> 16));
}
/* What a slot of a key that isn't an integer keeps of its mixed hash: the 30 bits that place it
 * in any table, with the top bit set, so no full slot's word is 0. */
CX_INLINE cx_u32 cx_oh_tag(cx_u32 h) { return (h & 0x3FFFFFFFu) | 0x80000000u; }
/* fastutil's constructor: the header for `expected` keys at the load factor `f`. */
cx_ohead cx_oh_new(cx_usize expected, cx_f32 f, const char* at);
/* A table made without a size becomes fastutil's default one, for 16 keys at 0.75, when it's
 * first used. */
CX_INLINE void cx_oh_ready(cx_ohead* h) {
    if (__builtin_expect(!h->n, 0)) *h = (cx_ohead){ 32, 31, 0, 24, 32, 0.75f, 0 };
}
/* `HashCommon.arraySize` and `maxFill`. */
cx_u32 cx_oh_array_size(cx_u64 expected, cx_f32 f);
cx_u32 cx_oh_max_fill(cx_u32 n, cx_f32 f);
/* The slots `putAll` and `addAll` make room in before adding `c` keys. */
cx_u32 cx_oh_reserve_size(const cx_ohead* h, cx_usize c);
/* The slots `trim()` shrinks the table to, or 0 when it stays. */
cx_u32 cx_oh_trim_size(const cx_ohead* h);

/* Bounds checks. */
CX_INLINE cx_usize cx_index(cx_usize i, cx_usize length, const char* at) {
    if (__builtin_expect(i >= length, 0)) cx_panic_index(i, length, at);
    return i;
}

/* Integer division (D55): division by zero panics, and MIN / -1 gives MIN, as in Java.
 * i8 and i16 divide as i32 and wrap back, so they can't overflow. */
#define CX_DIV_SMALL(T)                                                                  \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)((cx_i32)a / (cx_i32)b);                                          \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)((cx_i32)a % (cx_i32)b);                                          \
    }
#define CX_DIV_SIGNED(T, U)                                                              \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        if (b == -1) return (cx_##T)((U)0 - (U)a);                                       \
        return a / b;                                                                    \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        if (b == -1) return 0;                                                           \
        return a % b;                                                                    \
    }
#define CX_DIV_UNSIGNED(T)                                                               \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)(a / b);                                                          \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)(a % b);                                                          \
    }
CX_DIV_SMALL(i8)
CX_DIV_SMALL(i16)
CX_DIV_SIGNED(i32, cx_u32)
CX_DIV_SIGNED(i64, cx_u64)
CX_DIV_UNSIGNED(u8)
CX_DIV_UNSIGNED(u16)
CX_DIV_UNSIGNED(u32)
CX_DIV_UNSIGNED(u64)
CX_DIV_UNSIGNED(usize)

/* Java's Math.floorDiv and Math.floorMod. */
#define CX_FLOOR(T)                                                                      \
    CX_INLINE cx_##T cx_floor_div_##T(cx_##T a, cx_##T b, const char* at) {              \
        cx_##T q = cx_div_##T(a, b, at);                                                 \
        if ((a ^ b) < 0 && (cx_##T)(q * b) != a) q--;                                   \
        return q;                                                                        \
    }                                                                                    \
    CX_INLINE cx_##T cx_floor_mod_##T(cx_##T a, cx_##T b, const char* at) {              \
        cx_##T r = cx_rem_##T(a, b, at);                                                 \
        if ((r ^ b) < 0 && r != 0) r = (cx_##T)(r + b);                                  \
        return r;                                                                        \
    }
CX_FLOOR(i8)
CX_FLOOR(i16)
CX_FLOOR(i32)
CX_FLOOR(i64)

/* Java's float-to-integer casts: clamp to the range, and NaN gives 0. */
static inline cx_i32 cx_f64_to_i32_clamp(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return (cx_i32)d;
}
static inline cx_i64 cx_f64_to_i64_clamp(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return 9223372036854775807LL;
    if (d <= -9223372036854775808.0) return -9223372036854775807LL - 1;
    return (cx_i64)d;
}
static inline cx_i32 cx_f32_to_i32_clamp(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 2147483647;
    if (f <= -2147483648.0f) return -2147483647 - 1;
    return (cx_i32)f;
}
static inline cx_i64 cx_f32_to_i64_clamp(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 9223372036854775808.0f) return 9223372036854775807LL;
    if (f <= -9223372036854775808.0f) return -9223372036854775807LL - 1;
    return (cx_i64)f;
}
#if defined(__x86_64__)
/* x86's truncating conversions give the type's MIN for NaN and for anything out of range, so one
 * test of the result finds every case that needs clamping: one compare instead of three. */
typedef double cx_v2df __attribute__((vector_size(16)));
typedef float cx_v4sf __attribute__((vector_size(16)));
CX_INLINE cx_i32 cx_f64_to_i32(cx_f64 d) {
    cx_i32 r = __builtin_ia32_cvttsd2si((cx_v2df){d, 0.0});
    if (__builtin_expect(r == -2147483647 - 1, 0)) return cx_f64_to_i32_clamp(d);
    return r;
}
CX_INLINE cx_i64 cx_f64_to_i64(cx_f64 d) {
    cx_i64 r = __builtin_ia32_cvttsd2si64((cx_v2df){d, 0.0});
    if (__builtin_expect(r == -9223372036854775807LL - 1, 0)) return cx_f64_to_i64_clamp(d);
    return r;
}
CX_INLINE cx_i32 cx_f32_to_i32(cx_f32 f) {
    cx_i32 r = __builtin_ia32_cvttss2si((cx_v4sf){f, 0.0f, 0.0f, 0.0f});
    if (__builtin_expect(r == -2147483647 - 1, 0)) return cx_f32_to_i32_clamp(f);
    return r;
}
CX_INLINE cx_i64 cx_f32_to_i64(cx_f32 f) {
    cx_i64 r = __builtin_ia32_cvttss2si64((cx_v4sf){f, 0.0f, 0.0f, 0.0f});
    if (__builtin_expect(r == -9223372036854775807LL - 1, 0)) return cx_f32_to_i64_clamp(f);
    return r;
}
#else
CX_INLINE cx_i32 cx_f64_to_i32(cx_f64 d) { return cx_f64_to_i32_clamp(d); }
CX_INLINE cx_i64 cx_f64_to_i64(cx_f64 d) { return cx_f64_to_i64_clamp(d); }
CX_INLINE cx_i32 cx_f32_to_i32(cx_f32 f) { return cx_f32_to_i32_clamp(f); }
CX_INLINE cx_i64 cx_f32_to_i64(cx_f32 f) { return cx_f32_to_i64_clamp(f); }
#endif

/* Java's Math.round: the nearest integer, with halves rounded up, clamped as a cast is. x - floor(x)
 * is exact, except just below 0, where it can round up to 1, which still rounds to 0 as it should. */
CX_INLINE cx_i64 cx_round_f64(cx_f64 d) {
    cx_f64 f = __builtin_floor(d);
    return cx_f64_to_i64(d - f >= 0.5 ? f + 1.0 : f);
}
CX_INLINE cx_i32 cx_round_f32(cx_f32 x) {
    cx_f32 f = __builtin_floorf(x);
    return cx_f32_to_i32(x - f >= 0.5f ? f + 1.0f : f);
}

/* abs, min and max. Integer abs wraps, so abs(MIN) is MIN, as in Java. */
#define CX_INT_MINMAX(T)                                                                 \
    CX_INLINE cx_##T cx_min_##T(cx_##T a, cx_##T b) { return a <= b ? a : b; }           \
    CX_INLINE cx_##T cx_max_##T(cx_##T a, cx_##T b) { return a >= b ? a : b; }
#define CX_INT_ABS(T, U)                                                                 \
    CX_INLINE cx_##T cx_abs_##T(cx_##T a) { return (cx_##T)(a < 0 ? (U)0 - (U)a : (U)a); }
CX_INT_MINMAX(i8)
CX_INT_MINMAX(i16)
CX_INT_MINMAX(i32)
CX_INT_MINMAX(i64)
CX_INT_MINMAX(u8)
CX_INT_MINMAX(u16)
CX_INT_MINMAX(u32)
CX_INT_MINMAX(u64)
CX_INT_MINMAX(usize)
CX_INT_ABS(i8, cx_u32)
CX_INT_ABS(i16, cx_u32)
CX_INT_ABS(i32, cx_u32)
CX_INT_ABS(i64, cx_u64)

/* Java's Math.min and Math.max on floats: NaN if either is NaN, and -0.0 is less than 0.0. */
#define CX_FLOAT_MINMAX(T, SIGNBIT)                                                      \
    CX_INLINE cx_##T cx_min_##T(cx_##T a, cx_##T b) {                                    \
        if (a != a) return a;                                                            \
        if (b != b) return b;                                                            \
        if (a == b) return SIGNBIT(a) ? a : b;                                           \
        return a < b ? a : b;                                                            \
    }                                                                                    \
    CX_INLINE cx_##T cx_max_##T(cx_##T a, cx_##T b) {                                    \
        if (a != a) return a;                                                            \
        if (b != b) return b;                                                            \
        if (a == b) return SIGNBIT(a) ? b : a;                                           \
        return a > b ? a : b;                                                            \
    }
CX_FLOAT_MINMAX(f32, __builtin_signbit)
CX_FLOAT_MINMAX(f64, __builtin_signbit)
CX_INLINE cx_f32 cx_abs_f32(cx_f32 a) { return __builtin_fabsf(a); }
CX_INLINE cx_f64 cx_abs_f64(cx_f64 a) { return __builtin_fabs(a); }
/* Java's `Double.compare` and `Float.compare`, a total order: -0.0 comes before 0.0, and NaN after
 * everything, equal to itself. Values that `<` orders take the first two tests; for the rest,
 * every NaN is made the same, and the bits ordered as a signed integer, which puts -0.0 first. */
#define CX_TOTAL_CMP(T, I, U, NAN_BITS)                                                      CX_INLINE int cx_total_cmp_##T(cx_##T a, cx_##T b) {                                        if (a < b) return -1;                                                                    if (a > b) return 1;                                                                     I x, y;                                                                                  __builtin_memcpy(&x, &a, sizeof x);                                                      __builtin_memcpy(&y, &b, sizeof y);                                                      if (a != a) x = (I)(U)NAN_BITS;                                                          if (b != b) y = (I)(U)NAN_BITS;                                                          return (x > y) - (x < y);                                                            }
CX_TOTAL_CMP(f32, cx_i32, cx_u32, 0x7fc00000u)
CX_TOTAL_CMP(f64, cx_i64, cx_u64, 0x7ff8000000000000ull)

#endif
