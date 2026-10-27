/* OpenHashMap and OpenHashSet (D107): fastutil's open hash tables, in fastutil's order, for the
 * same operations.
 *
 * A fastutil table's order is its slots', from the last to the first, after the key 0 of an
 * integer table, and the slots are fixed by a few rules: a key goes in the first empty slot from
 * `mix(hash) & (n - 1)`; removing a key moves later keys of its run back into the gap; the table
 * rehashes, from its last slot down, to the size `arraySize` gives when an insert finds it at
 * `maxFill`, and to half its size when removing leaves it a quarter that full. This file is the
 * sizes those rules pick, written from the rules fastutil 8.5.18 keeps and checked against it by
 * running both on the same operations: it's no port of fastutil's code. The compiler writes the
 * probing, removing and rehashing for each key type.
 */
#include "calx_rt.h"

#define MAX_SLOTS (1u << 30)

/* `(long) Math.ceil(x)`, for the sizes here, which are at least 0: exact below 2^62, and 2^62
 * past it, which is more than any table holds. */
static cx_u64 ceil_count(double x) {
    if (!(x > 0)) return 0;
    if (x >= 4611686018427387904.0) return 1ull << 62;
    cx_u64 i = (cx_u64)x;
    return (double)i < x ? i + 1 : i;
}

/* `HashCommon.nextPowerOfTwo`: the power of two at least `x`, which is 1 for 0 and 1. */
static cx_u64 pow2_at_least(cx_u64 x) { return x <= 1 ? 1 : 1ull << (64 - __builtin_clzll(x - 1)); }

/* fastutil works these out in `double`, from the `float` load factor. */
cx_u32 cx_oh_array_size(cx_u64 expected, cx_f32 f) {
    cx_u64 s = pow2_at_least(ceil_count((double)expected / (double)f));
    if (s < 2) s = 2;
    if (s > MAX_SLOTS) cx_panic_table_size("<table>");
    return (cx_u32)s;
}

cx_u32 cx_oh_max_fill(cx_u32 n, cx_f32 f) {
    cx_u64 m = ceil_count((double)n * (double)f);
    return m < n - 1 ? (cx_u32)m : n - 1;
}

cx_ohead cx_oh_new(cx_usize expected, cx_f32 f, const char* at) {
    if (!(f > 0 && f < 1)) cx_panic_load_factor(at);
    if (expected > 0x7FFFFFFFu) cx_panic_table_size(at);
    cx_u32 n = cx_oh_array_size(expected, f);
    return (cx_ohead){ n, n - 1, 0, cx_oh_max_fill(n, f), n, f, 0 };
}

/* `putAll` and `addAll`: with a load factor of at most a half, `ensureCapacity(c)`, room for the
 * `c` keys alone; above that, `tryCapacity(len + c)`, which divides in `float`. */
cx_u32 cx_oh_reserve_size(const cx_ohead* h, cx_usize c) {
    if (h->f <= 0.5f) {
        if (c > 0x7FFFFFFFu) cx_panic_table_size("<table>");
        return cx_oh_array_size(c, h->f);
    }
    cx_u64 s = pow2_at_least(ceil_count((double)((cx_f32)((cx_u64)h->len + c) / h->f)));
    if (s < 2) s = 2;
    return s > MAX_SLOTS ? MAX_SLOTS : (cx_u32)s;
}

/* `trim()`: the power of two at least `len / f`, worked out in `float`, if it's smaller than the
 * table and holds the keys. */
cx_u32 cx_oh_trim_size(const cx_ohead* h) {
    cx_u64 l = pow2_at_least(ceil_count((double)((cx_f32)h->len / h->f)));
    if (l >= h->n || h->len > cx_oh_max_fill((cx_u32)l, h->f)) return 0;
    return (cx_u32)l;
}
