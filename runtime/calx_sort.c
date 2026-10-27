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
