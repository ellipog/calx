/* The sorts of lists of integers, `char`s, `rune`s and `bool`s (D96), a file of their own so only
 * programs that sort them compile and link it. Each is calx_sort.h with the element's key for
 * radix sorting: an integer's flips the sign bit, so negative values come first as unsigned keys. */
#include "calx_rt.h"

#define CX_SORT_T cx_i8
#define CX_SORT_NAME cx_sort_i8
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u8)(x) ^ 0x80u)
#define CX_SORT_KEY_T cx_u8
#define CX_SORT_UNKEY(k) ((cx_i8)((k) ^ 0x80u))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_i16
#define CX_SORT_NAME cx_sort_i16
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u16)(x) ^ 0x8000u)
#define CX_SORT_KEY_T cx_u16
#define CX_SORT_UNKEY(k) ((cx_i16)((k) ^ 0x8000u))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_i32
#define CX_SORT_NAME cx_sort_i32
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u32)(x) ^ 0x80000000u)
#define CX_SORT_KEY_T cx_u32
#define CX_SORT_UNKEY(k) ((cx_i32)((k) ^ 0x80000000u))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_i64
#define CX_SORT_NAME cx_sort_i64
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u64)(x) ^ 0x8000000000000000ull)
#define CX_SORT_KEY_T cx_u64
#define CX_SORT_UNKEY(k) ((cx_i64)((k) ^ 0x8000000000000000ull))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_u8
#define CX_SORT_NAME cx_sort_u8
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u8)(x))
#define CX_SORT_KEY_T cx_u8
#define CX_SORT_UNKEY(k) ((cx_u8)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_u16
#define CX_SORT_NAME cx_sort_u16
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u16)(x))
#define CX_SORT_KEY_T cx_u16
#define CX_SORT_UNKEY(k) ((cx_u16)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_u32
#define CX_SORT_NAME cx_sort_u32
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u32)(x))
#define CX_SORT_KEY_T cx_u32
#define CX_SORT_UNKEY(k) ((cx_u32)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_u64
#define CX_SORT_NAME cx_sort_u64
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u64)(x))
#define CX_SORT_KEY_T cx_u64
#define CX_SORT_UNKEY(k) ((cx_u64)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_usize
#define CX_SORT_NAME cx_sort_usize
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_usize)(x))
#define CX_SORT_KEY_T cx_usize
#define CX_SORT_UNKEY(k) ((cx_usize)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_char
#define CX_SORT_NAME cx_sort_char
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u16)(x))
#define CX_SORT_KEY_T cx_u16
#define CX_SORT_UNKEY(k) ((cx_char)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_rune
#define CX_SORT_NAME cx_sort_rune
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u32)(x))
#define CX_SORT_KEY_T cx_u32
#define CX_SORT_UNKEY(k) ((cx_rune)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"
#define CX_SORT_T cx_bool
#define CX_SORT_NAME cx_sort_bool
#define CX_SORT_LESS(a, b) (*(a) < *(b))
#define CX_SORT_KEY(x) ((cx_u8)(x))
#define CX_SORT_KEY_T cx_u8
#define CX_SORT_UNKEY(k) ((cx_bool)(k))
#define CX_SORT_NETWORK
#include "calx_sort.h"

/* `xs.sort_by(|a, b| a.total_cmp(b))` on a list of floats, and `b.total_cmp(a)` with `descending`
 * (CLX-292), which the compiler calls in place of its stable sort with the closure: radix sorting
 * them as integers takes a fifth of the time. total_cmp calls every NaN equal and puts them after
 * everything, so they go last, or first descending, in the order they were in, as the stable sort
 * leaves them. Every other value becomes its key in place: its bits, with a negative value's all
 * flipped and a positive one's sign, which order as unsigned integers as the values do, -0.0
 * before 0.0. The keys are sorted as integers are, flipped all over for descending, and turned
 * back. Two values with the same key have the same bits, so no program can tell which way equal
 * ones were sorted. */
#define CX_TOTAL_SORT(T, U, TOP, INTS)                                                       \
    void cx_sort_total_##T(cx_##T* v, cx_usize n, cx_bool descending) {                      \
        cx_usize nans = 0;                                                                    \
        for (cx_usize i = 0; i < n; i++) nans += __builtin_isnan(v[i]) != 0;                  \
        cx_usize from = 0;                                                                    \
        if (nans) {                                                                           \
            cx_##T* set = (cx_##T*)cx_alloc(nans * sizeof(cx_##T));                           \
            cx_usize q = 0;                                                                   \
            if (!descending) {                                                                \
                cx_usize j = 0;                                                               \
                for (cx_usize i = 0; i < n; i++) {                                            \
                    if (__builtin_isnan(v[i])) set[q++] = v[i];                               \
                    else v[j++] = v[i];                                                       \
                }                                                                             \
                __builtin_memcpy(v + j, set, nans * sizeof(cx_##T));                          \
            } else {                                                                          \
                cx_usize j = n;                                                               \
                for (cx_usize i = n; i-- > 0;) {                                              \
                    if (__builtin_isnan(v[i])) set[nans - 1 - q++] = v[i];                    \
                    else v[--j] = v[i];                                                       \
                }                                                                             \
                __builtin_memcpy(v, set, nans * sizeof(cx_##T));                              \
                from = nans;                                                                  \
            }                                                                                 \
            cx_free(set, nans * sizeof(cx_##T));                                              \
        }                                                                                     \
        cx_usize m = n - nans;                                                                \
        U* k = (U*)(v + from);                                                                \
        const U sign = (U)1 << TOP, all = descending ? (U)~(U)0 : 0;                          \
        for (cx_usize i = 0; i < m; i++) {                                                    \
            U b;                                                                              \
            __builtin_memcpy(&b, &k[i], sizeof b);                                            \
            b ^= ((U)0 - (b >> TOP)) | sign;                                                  \
            k[i] = b ^ all;                                                                   \
        }                                                                                     \
        INTS(k, m);                                                                           \
        /* Written back as floats, which is what the list is read as next. */                 \
        for (cx_usize i = 0; i < m; i++) {                                                    \
            U b = k[i] ^ all;                                                                 \
            b = (b >> TOP) ? b ^ sign : ~b;                                                   \
            cx_##T f;                                                                         \
            __builtin_memcpy(&f, &b, sizeof f);                                               \
            v[from + i] = f;                                                                  \
        }                                                                                     \
    }
CX_TOTAL_SORT(f32, cx_u32, 31, cx_sort_u32)
CX_TOTAL_SORT(f64, cx_u64, 63, cx_sort_u64)
