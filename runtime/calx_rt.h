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

/* A panic: `cx_panic_begin` starts the message on standard error, the print functions add to
 * it, and `cx_panic_end` finishes it and exits with status 101. */
void cx_panic_begin(const char* at);
CX_COLD void cx_panic_end(void);
CX_COLD void cx_panic_index(cx_usize index, cx_usize length, const char* at);
CX_COLD void cx_panic_div_zero(const char* at);
CX_COLD void cx_panic_unwrap(const char* at);

/* Whether two strings hold the same bytes, for string patterns. */
CX_INLINE cx_bool cx_str_eq(cx_str a, cx_str b) {
    return a.len == b.len && (a.len == 0 || __builtin_memcmp(a.ptr, b.ptr, a.len) == 0);
}

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
