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
 * isn't buffered. Output waiting for standard output goes first, so the two keep their order. */
void cx_err_begin(void);
void cx_err_end(void);

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

/* Memory. Running out panics. */
void* cx_alloc(cx_usize size);
void* cx_realloc(void* p, cx_usize size);
void cx_free(void* p);
/* Makes room for `extra` more elements of `size` bytes in a list's buffer of `*cap`, holding
 * `len`, growing it to at least twice its size. Returns the buffer, which may have moved. */
void* cx_grow(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size);
CX_COLD void cx_panic_capacity(void);

/* Strings. */
CX_INLINE cx_String cx_string_lit(const char* p, cx_usize len) {
    cx_String s = {(cx_u8*)p, len, 0};
    return s;
}
CX_INLINE cx_str cx_sv(cx_String s) {
    cx_str v = {s.ptr, s.len};
    return v;
}
CX_INLINE void cx_drop_String(cx_String* s) {
    if (s->cap) cx_free(s->ptr);
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
CX_COLD void cx_panic_key(const char* at);

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
CX_INLINE cx_i32 cx_f64_to_i32(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return (cx_i32)d;
}
CX_INLINE cx_i64 cx_f64_to_i64(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return 9223372036854775807LL;
    if (d <= -9223372036854775808.0) return -9223372036854775807LL - 1;
    return (cx_i64)d;
}
CX_INLINE cx_i32 cx_f32_to_i32(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 2147483647;
    if (f <= -2147483648.0f) return -2147483647 - 1;
    return (cx_i32)f;
}
CX_INLINE cx_i64 cx_f32_to_i64(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 9223372036854775808.0f) return 9223372036854775807LL;
    if (f <= -9223372036854775808.0f) return -9223372036854775807LL - 1;
    return (cx_i64)f;
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

#endif
