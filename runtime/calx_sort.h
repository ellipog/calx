/* `xs.sort()`, as a template (D96). Define
 *
 *   CX_SORT_T           the element type
 *   CX_SORT_NAME        the function's name, which sorts `CX_SORT_T* v` of length `cx_usize n`
 *   CX_SORT_LESS(a, b)  whether the element `a` points to comes before the one `b` points to.
 *                       Name the parameters so no field shares their names, as `cx_sa`
 *   CX_SORT_KEY(x)      optional: an unsigned integer that orders as the element `x` does, of
 *   CX_SORT_KEY_T       the key's type, the size of the element, and
 *   CX_SORT_UNKEY(k)    the element whose key is `k`. Long lists are then radix sorted
 *   CX_SORT_NETWORK     optional, with a key: sort short runs with sorting networks, for integers
 *   CX_SORT_STATIC      optional: make the function `static`
 *   CX_SORT_STABLE      optional: keep equal elements in the order they were in. The function
 *                       then takes `const char* at` too, where the sort is, for the panic
 *                       when an `Order` contradicts itself
 *
 * and include this file, once for each element type. It undefines them all.
 *
 * Without CX_SORT_STABLE, equal elements may change places: an `Order` that's derived compares
 * everything a value holds, so two equal elements are the same value, and no program can tell them
 * apart. That leaves the fastest algorithms free to use:
 *
 * - A list already in order is found in one pass and done. One strictly in reverse is reversed in
 *   the same pass, from both ends at once, checking as it goes (`_reversed`).
 * - Integers in lists of a few hundred or more are radix sorted (`_radix`). A first pass finds
 *   the bytes of the key that differ between elements. Keys of 4 bytes or more are then spread
 *   into buckets by the top bits that differ, about an element to a bucket, so every bucket comes
 *   before the next and one insertion pass finishes (`_spread`); a list of more than 65,536 goes
 *   into buckets of about 4,096, each then sorted the same way, in cache. When few bytes differ
 *   in a long list, a pass for each, from the lowest, is faster: each moves every element by its
 *   byte, with no comparisons. Values that would fill one bucket, as when they're spread over
 *   many powers of two, are quicksorted instead.
 * - Everything else is quicksorted. The pivot is the median of 3 elements, or of 3 medians of 3
 *   in longer runs. The partition has no branches on the comparison, so a mispredicted branch
 *   doesn't cost a pipeline flush on every other element. When a pivot equals the one the run's
 *   values all come after, the run holds many copies of it, and they're set aside in one pass,
 *   so lists with few distinct values take linear time. Runs of up to 16 are insertion sorted,
 *   or for integers runs of up to 32 are sorted by a network, with no branches either (`_leaf`),
 *   as are whole lists of integers shorter than 32 (`_tiny`), and a run that's been split too
 *   unevenly too often is heapsorted, so the worst case is O(n log n).
 *
 * An `Order` written by hand can call two different values equal, so a type that has one, or holds
 * one, is sorted stably (D102), as Java's `List.sort` is. That's a quicksort too, with the same
 * pivots and the same setting aside of a pivot's copies. Runs of up to 32 are sorted 4 at a time
 * by a network that keeps equal elements in order, then merged in pairs (`_small_stable`). Its partition keeps them in order as well: each element is
 * written to a buffer, those going left from its front and the rest from its back, choosing which
 * by pointer, with no branch; then the left ones are copied back, and the right ones copied back
 * in reverse. A run split too unevenly too often is merge sorted.
 *
 * Moving an element copies its bytes, as every move in Calx does, so nothing is copied deeply,
 * dropped or freed. The only memory used is a radix sort's buffer, the size of the list, and its
 * counts, 4 bytes a bucket, or the stable sort's buffer, the size of the list. */

#if defined(CX_SORT_NETWORK) && !defined(__OPTIMIZE__)
// A debug build doesn't optimize, and there a network is many times the code of the insertion
// sort it replaces, linked into every program that sorts integers, and no faster. Integers sort
// the same either way.
#undef CX_SORT_NETWORK
#endif

#define CX_SORT_CAT2(a, b) a##b
#define CX_SORT_CAT(a, b) CX_SORT_CAT2(a, b)
#define CX_SORT_F(x) CX_SORT_CAT(CX_SORT_NAME, x)
#define CX_SORT_SWAP(a, b)  \
    do {                    \
        CX_SORT_T t_ = (a); \
        (a) = (b);          \
        (b) = t_;           \
    } while (0)

static void CX_SORT_F(_insertion)(CX_SORT_T* v, cx_usize n) {
    for (cx_usize i = 1; i < n; i++) {
        if (!CX_SORT_LESS(&v[i], &v[i - 1])) continue;
        CX_SORT_T x = v[i];
        cx_usize j = i;
        do {
            v[j] = v[j - 1];
            j--;
        } while (j > 0 && CX_SORT_LESS(&x, &v[j - 1]));
        v[j] = x;
    }
}

/* The index of the median of v[a], v[b] and v[c]. */
static inline cx_usize CX_SORT_F(_median)(const CX_SORT_T* v, cx_usize a, cx_usize b, cx_usize c) {
    int ab = CX_SORT_LESS(&v[a], &v[b]), ac = CX_SORT_LESS(&v[a], &v[c]);
    if (ab != ac) return a;  // v[a] is between the other two
    // v[a] is the least or the greatest, so the median is the lesser or the greater of the others.
    return CX_SORT_LESS(&v[b], &v[c]) == ab ? b : c;
}

static inline cx_usize CX_SORT_F(_pivot)(const CX_SORT_T* v, cx_usize n) {
    cx_usize e = n / 8, a = 0, b = e * 4, c = e * 7;
    if (n >= 64) {
        cx_usize s = e / 2;
        a = CX_SORT_F(_median)(v, a, a + s, a + 2 * s);
        b = CX_SORT_F(_median)(v, b - s, b, b + s);
        c = CX_SORT_F(_median)(v, c - s, c, c + s);
    }
    return CX_SORT_F(_median)(v, a, b, c);
}

#ifndef CX_SORT_STABLE
#ifdef CX_SORT_NETWORK
static inline void CX_SORT_F(_order2)(CX_SORT_T* x, CX_SORT_T* y) {
    int c = CX_SORT_LESS(y, x);
    CX_SORT_T lo = *(c ? y : x), hi = *(c ? x : y);
    *x = lo;
    *y = hi;
}

/* Puts the lesser of two variables first, by choosing rather than branching. */
#define CX_SORT_CAS(a, b)                                     \
    do {                                                      \
        int c_ = CX_SORT_LESS(&(b), &(a));                    \
        CX_SORT_T lo_ = c_ ? (b) : (a), hi_ = c_ ? (a) : (b); \
        (a) = lo_;                                            \
        (b) = hi_;                                            \
    } while (0)

/* Generated by tools/sort_networks.py. */
static inline void CX_SORT_F(_net4)(CX_SORT_T* v) {
    CX_SORT_T x0 = v[0], x1 = v[1], x2 = v[2], x3 = v[3];
    CX_SORT_CAS(x0, x1); CX_SORT_CAS(x2, x3);
    CX_SORT_CAS(x0, x2); CX_SORT_CAS(x1, x3);
    CX_SORT_CAS(x1, x2);
    v[0] = x0; v[1] = x1; v[2] = x2; v[3] = x3;
}

static inline void CX_SORT_F(_net8)(CX_SORT_T* v) {
    CX_SORT_T x0 = v[0], x1 = v[1], x2 = v[2], x3 = v[3], x4 = v[4], x5 = v[5], x6 = v[6], x7 = v[7];
    CX_SORT_CAS(x0, x1); CX_SORT_CAS(x2, x3); CX_SORT_CAS(x4, x5); CX_SORT_CAS(x6, x7);
    CX_SORT_CAS(x0, x2); CX_SORT_CAS(x1, x3); CX_SORT_CAS(x4, x6); CX_SORT_CAS(x5, x7);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x0, x4); CX_SORT_CAS(x3, x7);
    CX_SORT_CAS(x2, x6); CX_SORT_CAS(x1, x5);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x3, x5);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6);
    v[0] = x0; v[1] = x1; v[2] = x2; v[3] = x3; v[4] = x4; v[5] = x5; v[6] = x6; v[7] = x7;
}

static inline void CX_SORT_F(_net16)(CX_SORT_T* v) {
    CX_SORT_T x0 = v[0], x1 = v[1], x2 = v[2], x3 = v[3], x4 = v[4], x5 = v[5], x6 = v[6], x7 = v[7],
              x8 = v[8], x9 = v[9], x10 = v[10], x11 = v[11], x12 = v[12], x13 = v[13], x14 = v[14],
              x15 = v[15];
    CX_SORT_CAS(x0, x1); CX_SORT_CAS(x2, x3); CX_SORT_CAS(x4, x5); CX_SORT_CAS(x6, x7); CX_SORT_CAS(x8, x9);
    CX_SORT_CAS(x10, x11); CX_SORT_CAS(x12, x13); CX_SORT_CAS(x14, x15);
    CX_SORT_CAS(x0, x2); CX_SORT_CAS(x1, x3); CX_SORT_CAS(x4, x6); CX_SORT_CAS(x5, x7); CX_SORT_CAS(x8, x10);
    CX_SORT_CAS(x9, x11); CX_SORT_CAS(x12, x14); CX_SORT_CAS(x13, x15);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x0, x4); CX_SORT_CAS(x3, x7); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x13, x14); CX_SORT_CAS(x8, x12); CX_SORT_CAS(x11, x15);
    CX_SORT_CAS(x2, x6); CX_SORT_CAS(x1, x5); CX_SORT_CAS(x10, x14); CX_SORT_CAS(x9, x13);
    CX_SORT_CAS(x0, x8); CX_SORT_CAS(x7, x15);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x3, x5); CX_SORT_CAS(x10, x12); CX_SORT_CAS(x11, x13);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x11, x12); CX_SORT_CAS(x13, x14);
    CX_SORT_CAS(x4, x12); CX_SORT_CAS(x2, x10); CX_SORT_CAS(x6, x14); CX_SORT_CAS(x1, x9);
    CX_SORT_CAS(x5, x13); CX_SORT_CAS(x3, x11);
    CX_SORT_CAS(x4, x8); CX_SORT_CAS(x6, x10); CX_SORT_CAS(x5, x9); CX_SORT_CAS(x7, x11);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x6, x8); CX_SORT_CAS(x10, x12); CX_SORT_CAS(x3, x5);
    CX_SORT_CAS(x7, x9); CX_SORT_CAS(x11, x13);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x7, x8); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x11, x12); CX_SORT_CAS(x13, x14);
    v[0] = x0; v[1] = x1; v[2] = x2; v[3] = x3; v[4] = x4; v[5] = x5; v[6] = x6; v[7] = x7; v[8] = x8;
    v[9] = x9; v[10] = x10; v[11] = x11; v[12] = x12; v[13] = x13; v[14] = x14; v[15] = x15;
}

static inline void CX_SORT_F(_net32)(CX_SORT_T* v) {
    CX_SORT_T x0 = v[0], x1 = v[1], x2 = v[2], x3 = v[3], x4 = v[4], x5 = v[5], x6 = v[6], x7 = v[7],
              x8 = v[8], x9 = v[9], x10 = v[10], x11 = v[11], x12 = v[12], x13 = v[13], x14 = v[14],
              x15 = v[15], x16 = v[16], x17 = v[17], x18 = v[18], x19 = v[19], x20 = v[20], x21 = v[21],
              x22 = v[22], x23 = v[23], x24 = v[24], x25 = v[25], x26 = v[26], x27 = v[27], x28 = v[28],
              x29 = v[29], x30 = v[30], x31 = v[31];
    CX_SORT_CAS(x0, x1); CX_SORT_CAS(x2, x3); CX_SORT_CAS(x4, x5); CX_SORT_CAS(x6, x7); CX_SORT_CAS(x8, x9);
    CX_SORT_CAS(x10, x11); CX_SORT_CAS(x12, x13); CX_SORT_CAS(x14, x15); CX_SORT_CAS(x16, x17);
    CX_SORT_CAS(x18, x19); CX_SORT_CAS(x20, x21); CX_SORT_CAS(x22, x23); CX_SORT_CAS(x24, x25);
    CX_SORT_CAS(x26, x27); CX_SORT_CAS(x28, x29); CX_SORT_CAS(x30, x31);
    CX_SORT_CAS(x0, x2); CX_SORT_CAS(x1, x3); CX_SORT_CAS(x4, x6); CX_SORT_CAS(x5, x7); CX_SORT_CAS(x8, x10);
    CX_SORT_CAS(x9, x11); CX_SORT_CAS(x12, x14); CX_SORT_CAS(x13, x15); CX_SORT_CAS(x16, x18);
    CX_SORT_CAS(x17, x19); CX_SORT_CAS(x20, x22); CX_SORT_CAS(x21, x23); CX_SORT_CAS(x24, x26);
    CX_SORT_CAS(x25, x27); CX_SORT_CAS(x28, x30); CX_SORT_CAS(x29, x31);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x0, x4); CX_SORT_CAS(x3, x7); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x13, x14); CX_SORT_CAS(x8, x12); CX_SORT_CAS(x11, x15); CX_SORT_CAS(x17, x18);
    CX_SORT_CAS(x21, x22); CX_SORT_CAS(x16, x20); CX_SORT_CAS(x19, x23); CX_SORT_CAS(x25, x26);
    CX_SORT_CAS(x29, x30); CX_SORT_CAS(x24, x28); CX_SORT_CAS(x27, x31);
    CX_SORT_CAS(x2, x6); CX_SORT_CAS(x1, x5); CX_SORT_CAS(x10, x14); CX_SORT_CAS(x9, x13);
    CX_SORT_CAS(x0, x8); CX_SORT_CAS(x7, x15); CX_SORT_CAS(x18, x22); CX_SORT_CAS(x17, x21);
    CX_SORT_CAS(x26, x30); CX_SORT_CAS(x25, x29); CX_SORT_CAS(x16, x24); CX_SORT_CAS(x23, x31);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x3, x5); CX_SORT_CAS(x10, x12); CX_SORT_CAS(x11, x13);
    CX_SORT_CAS(x18, x20); CX_SORT_CAS(x19, x21); CX_SORT_CAS(x26, x28); CX_SORT_CAS(x27, x29);
    CX_SORT_CAS(x0, x16); CX_SORT_CAS(x15, x31);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x11, x12); CX_SORT_CAS(x13, x14); CX_SORT_CAS(x17, x18); CX_SORT_CAS(x19, x20);
    CX_SORT_CAS(x21, x22); CX_SORT_CAS(x25, x26); CX_SORT_CAS(x27, x28); CX_SORT_CAS(x29, x30);
    CX_SORT_CAS(x4, x12); CX_SORT_CAS(x2, x10); CX_SORT_CAS(x6, x14); CX_SORT_CAS(x1, x9);
    CX_SORT_CAS(x5, x13); CX_SORT_CAS(x3, x11); CX_SORT_CAS(x20, x28); CX_SORT_CAS(x18, x26);
    CX_SORT_CAS(x22, x30); CX_SORT_CAS(x17, x25); CX_SORT_CAS(x21, x29); CX_SORT_CAS(x19, x27);
    CX_SORT_CAS(x4, x8); CX_SORT_CAS(x6, x10); CX_SORT_CAS(x5, x9); CX_SORT_CAS(x7, x11);
    CX_SORT_CAS(x20, x24); CX_SORT_CAS(x22, x26); CX_SORT_CAS(x21, x25); CX_SORT_CAS(x23, x27);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x6, x8); CX_SORT_CAS(x10, x12); CX_SORT_CAS(x3, x5);
    CX_SORT_CAS(x7, x9); CX_SORT_CAS(x11, x13); CX_SORT_CAS(x18, x20); CX_SORT_CAS(x22, x24);
    CX_SORT_CAS(x26, x28); CX_SORT_CAS(x19, x21); CX_SORT_CAS(x23, x25); CX_SORT_CAS(x27, x29);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x7, x8); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x11, x12); CX_SORT_CAS(x13, x14); CX_SORT_CAS(x17, x18); CX_SORT_CAS(x19, x20);
    CX_SORT_CAS(x21, x22); CX_SORT_CAS(x23, x24); CX_SORT_CAS(x25, x26); CX_SORT_CAS(x27, x28);
    CX_SORT_CAS(x29, x30);
    CX_SORT_CAS(x8, x24); CX_SORT_CAS(x4, x20); CX_SORT_CAS(x12, x28); CX_SORT_CAS(x2, x18);
    CX_SORT_CAS(x10, x26); CX_SORT_CAS(x6, x22); CX_SORT_CAS(x14, x30); CX_SORT_CAS(x1, x17);
    CX_SORT_CAS(x9, x25); CX_SORT_CAS(x5, x21); CX_SORT_CAS(x13, x29); CX_SORT_CAS(x3, x19);
    CX_SORT_CAS(x11, x27); CX_SORT_CAS(x7, x23);
    CX_SORT_CAS(x8, x16); CX_SORT_CAS(x12, x20); CX_SORT_CAS(x10, x18); CX_SORT_CAS(x14, x22);
    CX_SORT_CAS(x9, x17); CX_SORT_CAS(x13, x21); CX_SORT_CAS(x11, x19); CX_SORT_CAS(x15, x23);
    CX_SORT_CAS(x4, x8); CX_SORT_CAS(x12, x16); CX_SORT_CAS(x20, x24); CX_SORT_CAS(x6, x10);
    CX_SORT_CAS(x14, x18); CX_SORT_CAS(x22, x26); CX_SORT_CAS(x5, x9); CX_SORT_CAS(x13, x17);
    CX_SORT_CAS(x21, x25); CX_SORT_CAS(x7, x11); CX_SORT_CAS(x15, x19); CX_SORT_CAS(x23, x27);
    CX_SORT_CAS(x2, x4); CX_SORT_CAS(x6, x8); CX_SORT_CAS(x10, x12); CX_SORT_CAS(x14, x16);
    CX_SORT_CAS(x18, x20); CX_SORT_CAS(x22, x24); CX_SORT_CAS(x26, x28); CX_SORT_CAS(x3, x5);
    CX_SORT_CAS(x7, x9); CX_SORT_CAS(x11, x13); CX_SORT_CAS(x15, x17); CX_SORT_CAS(x19, x21);
    CX_SORT_CAS(x23, x25); CX_SORT_CAS(x27, x29);
    CX_SORT_CAS(x1, x2); CX_SORT_CAS(x3, x4); CX_SORT_CAS(x5, x6); CX_SORT_CAS(x7, x8); CX_SORT_CAS(x9, x10);
    CX_SORT_CAS(x11, x12); CX_SORT_CAS(x13, x14); CX_SORT_CAS(x15, x16); CX_SORT_CAS(x17, x18);
    CX_SORT_CAS(x19, x20); CX_SORT_CAS(x21, x22); CX_SORT_CAS(x23, x24); CX_SORT_CAS(x25, x26);
    CX_SORT_CAS(x27, x28); CX_SORT_CAS(x29, x30);
    v[0] = x0; v[1] = x1; v[2] = x2; v[3] = x3; v[4] = x4; v[5] = x5; v[6] = x6; v[7] = x7; v[8] = x8;
    v[9] = x9; v[10] = x10; v[11] = x11; v[12] = x12; v[13] = x13; v[14] = x14; v[15] = x15; v[16] = x16;
    v[17] = x17; v[18] = x18; v[19] = x19; v[20] = x20; v[21] = x21; v[22] = x22; v[23] = x23; v[24] = x24;
    v[25] = x25; v[26] = x26; v[27] = x27; v[28] = x28; v[29] = x29; v[30] = x30; v[31] = x31;
}

/* Sorts v[0..n), n <= 32, a run of a quicksort or a bucket of a spread, in a list of at least 32
 * that ends at `end`, with no branch on a comparison (D157): by the network for 4, 8, 16 or 32
 * elements, the shortest that holds the run, on that many from v[0], or on the last of the list if
 * they'd go past its end. The neighbors it takes in stay among their own: every element before the
 * run is at most every element in it, and every element after at least, and integers that are
 * equal are the same, so they're only reordered among themselves, which the sorting still to come
 * doesn't mind. That's faster than insertion, whose every element costs a mispredicted branch,
 * and needs no copy. */
static inline void CX_SORT_F(_leaf)(CX_SORT_T* v, cx_usize n, CX_SORT_T* end) {
    if (n <= 2) {
        if (n == 2) CX_SORT_F(_order2)(v, v + 1);
    } else if (n <= 4) {
        CX_SORT_F(_net4)(end - v >= 4 ? v : end - 4);
    } else if (n <= 8) {
        CX_SORT_F(_net8)(end - v >= 8 ? v : end - 8);
    } else if (n <= 16) {
        CX_SORT_F(_net16)(end - v >= 16 ? v : end - 16);
    } else {
        CX_SORT_F(_net32)(end - v >= 32 ? v : end - 32);
    }
}

/* Sorts v[0..n), half < n < 2 * half, by the network for 2 * half on a copy that holds the type's
 * greatest value after the elements. Each way, the copy is two of `half` elements, which overlap,
 * so neither loops. */
static inline __attribute__((always_inline)) void CX_SORT_F(_padded)(CX_SORT_T* v, cx_usize n, cx_usize half) {
    CX_SORT_T b[32];
    CX_SORT_T top = CX_SORT_UNKEY((CX_SORT_KEY_T)~(CX_SORT_KEY_T)0);
    for (cx_usize k = half; k < 2 * half; k++) b[k] = top;
    __builtin_memcpy(b, v, half * sizeof(CX_SORT_T));
    __builtin_memcpy(b + n - half, v + n - half, half * sizeof(CX_SORT_T));
    if (half == 4) CX_SORT_F(_net8)(b);
    else if (half == 8) CX_SORT_F(_net16)(b);
    else CX_SORT_F(_net32)(b);
    __builtin_memcpy(v, b, half * sizeof(CX_SORT_T));
    __builtin_memcpy(v + n - half, b + n - half, half * sizeof(CX_SORT_T));
}

/* Sorts a whole list of 2 to 31 elements, by the network of its length, or by the next one up. */
static void CX_SORT_F(_tiny)(CX_SORT_T* v, cx_usize n) {
    if (n == 2) {
        CX_SORT_F(_order2)(v, v + 1);
    } else if (n == 3) {
        CX_SORT_T x0 = v[0], x1 = v[1], x2 = v[2];
        CX_SORT_CAS(x0, x2);
        CX_SORT_CAS(x0, x1);
        CX_SORT_CAS(x1, x2);
        v[0] = x0, v[1] = x1, v[2] = x2;
    } else if (n == 4) {
        CX_SORT_F(_net4)(v);
    } else if (n < 8) {
        CX_SORT_F(_padded)(v, n, 4);
    } else if (n == 8) {
        CX_SORT_F(_net8)(v);
    } else if (n < 16) {
        CX_SORT_F(_padded)(v, n, 8);
    } else if (n == 16) {
        CX_SORT_F(_net16)(v);
    } else {
        CX_SORT_F(_padded)(v, n, 16);
    }
}
#endif

static void CX_SORT_F(_sift)(CX_SORT_T* v, cx_usize n, cx_usize node) {
    for (;;) {
        cx_usize child = 2 * node + 1;
        if (child >= n) return;
        if (child + 1 < n && CX_SORT_LESS(&v[child], &v[child + 1])) child++;
        if (!CX_SORT_LESS(&v[node], &v[child])) return;
        CX_SORT_SWAP(v[node], v[child]);
        node = child;
    }
}

static void CX_SORT_F(_heap)(CX_SORT_T* v, cx_usize n) {
    for (cx_usize i = n / 2; i-- > 0;) CX_SORT_F(_sift)(v, n, i);
    for (cx_usize end = n; end-- > 1;) {
        CX_SORT_SWAP(v[0], v[end]);
        CX_SORT_F(_sift)(v, end, 0);
    }
}

/* Moves the elements of v[0..n) that go left, those less than the pivot, or with `le` those not
 * greater than it, to the front, and says how many there are. The pivot isn't in v.
 *
 * Small elements: the first is taken out, leaving a gap just behind the element being looked at.
 * Each element in turn goes where the left side ends, and what was there fills the gap, whichever
 * side the element belongs on; only whether the left side grows depends on the comparison.
 * Large elements are moved only when they must be, from both ends, as copying them costs more
 * than a mispredicted branch. */
static inline __attribute__((always_inline)) cx_usize CX_SORT_F(_partition)(CX_SORT_T* v, cx_usize n, const CX_SORT_T* pivot,
                                                                             int le) {
#define CX_SORT_LEFT(x) (le ? !CX_SORT_LESS(pivot, (x)) : CX_SORT_LESS((x), pivot))
    if (sizeof(CX_SORT_T) <= 32) {
        if (n == 0) return 0;
        CX_SORT_T taken = v[0];
        cx_usize left = 0;
        for (cx_usize i = 1; i < n; i++) {
            // Copied from memory, not from the fields the comparison loaded, so the element is
            // stored whole, and loading it again next time can take it straight from the store.
            int goes = CX_SORT_LEFT(&v[i]);
            v[i - 1] = v[left];
            v[left] = v[i];
            left += goes;
        }
        int goes = CX_SORT_LEFT(&taken);
        v[n - 1] = v[left];
        v[left] = taken;
        return left + goes;
    }
    cx_usize l = 0, r = n;
    for (;;) {
        while (l < r && CX_SORT_LEFT(&v[l])) l++;
        while (l < r && !CX_SORT_LEFT(&v[r - 1])) r--;
        if (l >= r) return l;
        r--;
        CX_SORT_SWAP(v[l], v[r]);
        l++;
    }
#undef CX_SORT_LEFT
}

/* Every value in v[0..n) is at least `*floor`, the pivot that set the run apart, if there is one.
 * `limit` is how many more partitions may go before heapsort takes over. With a network, the list
 * the run is in holds at least 32 elements, and ends at `end`. */
static void CX_SORT_F(_quick)(CX_SORT_T* v, cx_usize n, const CX_SORT_T* floor, int limit, CX_SORT_T* end) {
    for (;;) {
#ifdef CX_SORT_NETWORK
        if (n <= 32) {
            CX_SORT_F(_leaf)(v, n, end);
            return;
        }
#else
        if (n <= 16) {
            CX_SORT_F(_insertion)(v, n);
            return;
        }
#endif
        if (limit == 0) {
            CX_SORT_F(_heap)(v, n);
            return;
        }
        limit--;
        cx_usize p = CX_SORT_F(_pivot)(v, n);
        CX_SORT_SWAP(v[0], v[p]);
        if (floor && !CX_SORT_LESS(floor, &v[0])) {
            // The pivot equals the floor, so every value not greater than it equals it: set them
            // all aside, and sort what's greater.
            cx_usize same = CX_SORT_F(_partition)(v + 1, n - 1, &v[0], 1) + 1;
            v += same;
            n -= same;
            floor = 0;
            continue;
        }
        cx_usize mid = CX_SORT_F(_partition)(v + 1, n - 1, &v[0], 0);
        CX_SORT_SWAP(v[0], v[mid]);
        CX_SORT_F(_quick)(v, mid, floor, limit, end);
        floor = &v[mid];
        v += mid + 1;
        n -= mid + 1;
    }
}

#ifdef CX_SORT_KEY
static void CX_SORT_F(_core)(CX_SORT_T* v, cx_usize n, CX_SORT_T* end);

/* Sorts v[0..n), n >= 192, whose keys differ only in the bits set in `differ`, by spreading the
 * elements into buckets by the top of those bits, so every element of a bucket comes before every
 * element of the next (D157). There's about a bucket for every element, or in a list of more than
 * 65,536 one for every 4,096, so each bucket is a list that fits in cache, sorted the same way.
 * A bucket of more than 32 is sorted by itself, one of 5 to 32 by a network, and then one
 * insertion pass, which copies the elements back, sorts the rest, as no element moves past the
 * start of its bucket. Says whether it did: if more than half the elements would go to one bucket,
 * the buckets wouldn't split the list, and a quicksort does better. A sample of 32 finds that
 * before the counting, which is slow when the counts it adds to are the same few. */
static int CX_SORT_F(_spread)(CX_SORT_T* v, cx_usize n, CX_SORT_KEY_T differ) {
    typedef CX_SORT_KEY_T key_t_;
    int bits = 64 - __builtin_clzll((unsigned long long)differ);
    key_t_ mask = (key_t_)((key_t_)~(key_t_)0 >> (8 * sizeof(key_t_) - bits));
    int want = 64 - __builtin_clzll((unsigned long long)(n > 65536 ? n / 4096 : n - 1));
    if (want > 16) want = 16;
    int shift = bits > want ? bits - want : 0;
    cx_usize buckets = (cx_usize)1 << (bits - shift);
    // The sample is taken at the fractions k times the golden ratio, which never repeat, so a list
    // that does, as with every third element small, can't fall in step with it.
    cx_u32 sample[32];
    for (int k = 0; k < 32; k++) {
        unsigned long long f = (unsigned long long)(k + 1) * 0x9E3779B97F4A7C15ull >> 32;
        sample[k] = (cx_u32)((CX_SORT_KEY(v[(cx_usize)(f * n >> 32)]) & mask) >> shift);
    }
    for (int i = 0; i < 32; i++) {
        int same = 0;
        for (int j = 0; j < 32; j++) same += sample[j] == sample[i];
        if (same > 16) return 0;
    }
    cx_u32* count = (cx_u32*)cx_alloc(buckets * sizeof(cx_u32));
    __builtin_memset(count, 0, buckets * sizeof(cx_u32));
    for (cx_usize i = 0; i < n; i++) count[(CX_SORT_KEY(v[i]) & mask) >> shift]++;
    cx_u32 at = 0, most = 0;
    for (cx_usize b = 0; b < buckets; b++) {
        cx_u32 c = count[b];
        most = c > most ? c : most;
        count[b] = at;
        at += c;
    }
    if (most > n / 2) {
        cx_free(count, buckets * sizeof(cx_u32));
        return 0;
    }
    cx_usize bytes = n * sizeof(CX_SORT_T);
    CX_SORT_T* buf = (CX_SORT_T*)cx_alloc(bytes);
    for (cx_usize i = 0; i < n; i++) {
        CX_SORT_T x = v[i];
        buf[count[(CX_SORT_KEY(x) & mask) >> shift]++] = x;
    }
    // Each count is now where its bucket ends.
    cx_u32 start = 0;
    for (cx_usize b = 0; b < buckets; b++) {
        cx_u32 size = count[b] - start;
        if (size > 32) CX_SORT_F(_core)(buf + start, size, buf + n);
#ifdef CX_SORT_NETWORK
        else if (size > 4) CX_SORT_F(_leaf)(buf + start, size, buf + n);
#endif
        start = count[b];
    }
    v[0] = buf[0];
    for (cx_usize i = 1; i < n; i++) {
        CX_SORT_T x = buf[i];
        cx_usize j = i;
        while (j > 0 && CX_SORT_LESS(&x, &v[j - 1])) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
    cx_free(buf, bytes);
    cx_free(count, buckets * sizeof(cx_u32));
    return 1;
}

/* Radix sorts v[0..n) and says so, unless a quicksort would be faster. One pass finds the bytes
 * that differ between elements, which says which is faster. A long key is spread, unless few of
 * its bytes differ in a long list, where the passes below are faster. They count every byte, then
 * each moves the elements by one of the bytes that differ. With only one, the counts are all it
 * takes: each value is written as often as it was counted, in place. */
static int CX_SORT_F(_radix)(CX_SORT_T* v, cx_usize n) {
    typedef CX_SORT_KEY_T key_t_;
    enum { BYTES = sizeof(key_t_) };
    _Static_assert(sizeof(key_t_) == sizeof(CX_SORT_T), "a radix key is the element's size");
    key_t_ all = (key_t_)~(key_t_)0, any = 0;
    for (cx_usize i = 0; i < n; i++) {
        key_t_ k = CX_SORT_KEY(v[i]);
        all &= k;
        any |= k;
    }
    int used = 0, only = 0;
    for (int b = 0; b < BYTES; b++) {
        if ((((all ^ any) >> (8 * b)) & 255) != 0) used++, only = b;
    }
    if (used == 0) return 1;  // every element is the same
    if (used == 1) {
        if (n < 128) return 0;
        cx_usize count[256];
        __builtin_memset(count, 0, sizeof count);
        for (cx_usize i = 0; i < n; i++) count[(CX_SORT_KEY(v[i]) >> (8 * only)) & 255]++;
        key_t_ rest = all & (key_t_)~((key_t_)255 << (8 * only));
        cx_usize at = 0;
        for (int d = 0; d < 256; d++) {
            CX_SORT_T x = CX_SORT_UNKEY(rest | ((key_t_)d << (8 * only)));
            for (cx_usize end = at + count[d]; at < end; at++) v[at] = x;
        }
        return 1;
    }
    if (BYTES >= 4 && n >= 192 && (used >= 5 || n <= 4096 * BYTES)) return CX_SORT_F(_spread)(v, n, (key_t_)(all ^ any));
    if (n < (cx_usize)(128 * used * used)) return 0;
    // Every byte is counted, even those that don't differ, as a loop of fixed length is faster.
    cx_usize count[BYTES][256];
    __builtin_memset(count, 0, sizeof count);
    for (cx_usize i = 0; i < n; i++) {
        key_t_ k = CX_SORT_KEY(v[i]);
        for (int b = 0; b < BYTES; b++) count[b][(k >> (8 * b)) & 255]++;
    }
    cx_usize bytes = n * sizeof(CX_SORT_T);
    CX_SORT_T* buf = (CX_SORT_T*)cx_alloc(bytes);
    CX_SORT_T *from = v, *to = buf;
    for (int b = 0; b < BYTES; b++) {
        if ((((all ^ any) >> (8 * b)) & 255) == 0) continue;
        cx_usize* c = count[b];
        cx_usize at = 0;
        for (int d = 0; d < 256; d++) {
            cx_usize k = c[d];
            c[d] = at;
            at += k;
        }
        for (cx_usize i = 0; i < n; i++) {
            CX_SORT_T x = from[i];
            to[c[(CX_SORT_KEY(x) >> (8 * b)) & 255]++] = x;
        }
        CX_SORT_T* t = from;
        from = to;
        to = t;
    }
    if (from != v) __builtin_memcpy(v, from, bytes);
    cx_free(buf, bytes);
    return 1;
}
#endif

/* Sorts v[0..n), a list or a bucket of one, which ends at `end`. */
static void CX_SORT_F(_core)(CX_SORT_T* v, cx_usize n, CX_SORT_T* end) {
#ifdef CX_SORT_KEY
    if (n >= 128 && CX_SORT_F(_radix)(v, n)) return;
#endif
    CX_SORT_F(_quick)(v, n, 0, 2 * (63 - __builtin_clzll((unsigned long long)n)), end);
}

/* Reverses v[0..n), n >= 2, and says so, if it's strictly in reverse order. It swaps from both ends
 * at once, 16 elements a side, checking each block before it swaps it, so every element is read
 * and written once, where finding the order first and then reversing reads it twice. If the order
 * breaks, it stops, and v holds its elements in some other order, which the sort that follows
 * takes as well as any. */
static int CX_SORT_F(_reversed)(CX_SORT_T* v, cx_usize n) {
    cx_usize lo = 0, hi = n;  // v[lo..hi) is still to do, and lo + hi == n
    while (hi - lo >= 33) {
        // The front block's pairs up to the element after it, which isn't swapped yet, and the
        // back block's down to the one before it.
        int wrong = 0;
        for (int j = 1; j <= 16; j++) wrong |= !CX_SORT_LESS(&v[lo + j], &v[lo + j - 1]);
        for (int j = 0; j < 16; j++) wrong |= !CX_SORT_LESS(&v[hi - 16 + j], &v[hi - 17 + j]);
        if (wrong) return 0;
        for (int k = 0; k < 16; k++) CX_SORT_SWAP(v[lo + k], v[hi - 1 - k]);
        lo += 16;
        hi -= 16;
    }
    for (cx_usize j = lo + 1; j < hi; j++) {
        if (!CX_SORT_LESS(&v[j], &v[j - 1])) return 0;
    }
    for (cx_usize l = lo, r = hi - 1; l < r; l++, r--) CX_SORT_SWAP(v[l], v[r]);
    return 1;
}

#else
/* Merges v[0..mid) and v[mid..n), each in order, through `buf`, which holds `mid` elements. */
static void CX_SORT_F(_merge_stable)(CX_SORT_T* v, cx_usize mid, cx_usize n, CX_SORT_T* buf) {
    __builtin_memcpy(buf, v, mid * sizeof(CX_SORT_T));
    const CX_SORT_T *l = buf, *le = buf + mid, *r = v + mid, *re = v + n;
    CX_SORT_T* d = v;
    while (l < le && r < re) {
        int right = CX_SORT_LESS(r, l);
        *d++ = *(__builtin_unpredictable(right) ? r : l);
        r += right;
        l += !right;
    }
    // What's left of the right half is already in place.
    if (l < le) __builtin_memcpy(d, l, (cx_usize)(le - l) * sizeof(CX_SORT_T));
}

/* A merge sort, for a run the quicksort split too unevenly too often. */
static void CX_SORT_F(_merge_sort)(CX_SORT_T* v, cx_usize n, CX_SORT_T* buf) {
    if (n <= 20) {
        CX_SORT_F(_insertion)(v, n);
        return;
    }
    cx_usize mid = n / 2;
    CX_SORT_F(_merge_sort)(v, mid, buf);
    CX_SORT_F(_merge_sort)(v + mid, n - mid, buf);
    if (CX_SORT_LESS(&v[mid], &v[mid - 1])) CX_SORT_F(_merge_stable)(v, mid, n, buf);
}

/* Moves the elements of v[0..n) that go left, those less than the pivot, or with `le` those not
 * greater than it, to the front, and the rest after them, each in the order they were in. Says
 * how many go left. Those that do are written where the left side ends, which is never past the
 * element being read, and the rest to `buf`, which holds n elements, then copied back after them. */
static inline __attribute__((always_inline)) cx_usize CX_SORT_F(_split)(CX_SORT_T* v, cx_usize n, CX_SORT_T* buf,
                                                                         const CX_SORT_T* pivot, int le) {
    CX_SORT_T* lo = v;
    CX_SORT_T* hi = buf;
    for (cx_usize i = 0; i < n; i++) {
        int goes = le ? !CX_SORT_LESS(pivot, &v[i]) : CX_SORT_LESS(&v[i], pivot);
        *(__builtin_unpredictable(goes) ? lo : hi) = v[i];
        lo += goes;
        hi += !goes;
    }
    cx_usize mid = (cx_usize)(lo - v);
    __builtin_memcpy(lo, buf, (n - mid) * sizeof(CX_SORT_T));
    return mid;
}

/* The median of v[a], v[a + 4e] and v[a + 7e], each found the same way from the eighth of the
 * run around it while that's long enough: for a long run, a pivot from many more elements, so
 * the run splits more evenly, which matters more when every split copies half the run. */
static cx_usize CX_SORT_F(_median_far)(const CX_SORT_T* v, cx_usize a, cx_usize e) {
    cx_usize b = a + 4 * e, c = a + 7 * e;
    if (e >= 8) {
        cx_usize f = e / 8;
        a = CX_SORT_F(_median_far)(v, a, f);
        b = CX_SORT_F(_median_far)(v, b, f);
        c = CX_SORT_F(_median_far)(v, c, f);
    }
    return CX_SORT_F(_median)(v, a, b, c);
}

/* v[0..4) in order into d[0..4), equal elements in the order they were in, with no branch: two
 * ordered pairs, then the least of their firsts and the greatest of their seconds, then the two
 * left, which are ordered by where they came from before they're compared. */
static inline void CX_SORT_F(_sort4)(const CX_SORT_T* v, CX_SORT_T* d) {
    int x = CX_SORT_LESS(&v[1], &v[0]), y = CX_SORT_LESS(&v[3], &v[2]);
    const CX_SORT_T *a = v + x, *b = v + !x, *c = v + 2 + y, *e = v + 2 + !y;
    int p = __builtin_unpredictable(CX_SORT_LESS(c, a)), q = __builtin_unpredictable(CX_SORT_LESS(e, b));
    const CX_SORT_T* least = p ? c : a;
    const CX_SORT_T* most = q ? b : e;
    const CX_SORT_T* l = p ? a : q ? c : b;
    const CX_SORT_T* r = q ? e : p ? b : c;
    int z = CX_SORT_LESS(r, l);
    d[0] = *least;
    d[1] = *(__builtin_unpredictable(z) ? r : l);
    d[2] = *(__builtin_unpredictable(z) ? l : r);
    d[3] = *most;
}

/* Merges l[0..le) and r[0..re), each in order, into d, taking from the right only when it's
 * strictly less, by choosing a pointer. */
static inline void CX_SORT_F(_merge_into)(const CX_SORT_T* l, const CX_SORT_T* le, const CX_SORT_T* r,
                                          const CX_SORT_T* re, CX_SORT_T* d) {
    while (l < le && r < re) {
        int right = CX_SORT_LESS(r, l);
        *d++ = *(__builtin_unpredictable(right) ? r : l);
        r += right;
        l += !right;
    }
    while (l < le) *d++ = *l++;
    while (r < re) *d++ = *r++;
}

/* Merges v[0..h) and v[h..2h), each in order, into d, from both ends at once: the front takes
 * the lesser, the left one if they're equal, and the back the greater, the right one if they're
 * equal. The two don't wait on each other, so a step costs half as much. With runs of the same
 * length, each side's last step is the one that empties a run, so neither reads past its own run;
 * an `Order` that contradicts itself could make them, which the check at the end finds, before
 * an element that's been written twice can be dropped twice. */
static inline void CX_SORT_F(_merge_both)(const CX_SORT_T* v, cx_usize h, CX_SORT_T* d, const char* at) {
    const CX_SORT_T *l = v, *r = v + h, *lb = v + h - 1, *rb = v + 2 * h - 1;
    CX_SORT_T *df = d, *db = d + 2 * h - 1;
    for (cx_usize k = h; k; k--) {
        int f = CX_SORT_LESS(r, l);
        *df++ = *(__builtin_unpredictable(f) ? r : l);
        r += f;
        l += !f;
        int b = CX_SORT_LESS(rb, lb);
        *db-- = *(__builtin_unpredictable(b) ? lb : rb);
        lb -= b;
        rb -= !b;
    }
    if (l != lb + 1 || r != rb + 1) cx_panic_order(at);
}

/* Sorts up to 32 elements stably with no branch on a comparison, through `buf`: runs of 4 by
 * `_sort4`, what's left by insertion, then runs merged in pairs, back and forth, from both ends
 * when they're the same length. */
static void CX_SORT_F(_small_stable)(CX_SORT_T* v, cx_usize n, CX_SORT_T* buf, const char* at) {
    cx_usize k = 0;
    for (; k + 4 <= n; k += 4) CX_SORT_F(_sort4)(v + k, buf + k);
    if (k < n) {
        __builtin_memcpy(buf + k, v + k, (n - k) * sizeof(CX_SORT_T));
        CX_SORT_F(_insertion)(buf + k, n - k);
    }
    CX_SORT_T *from = buf, *to = v;
    for (cx_usize w = 4; w < n; w *= 2) {
        for (cx_usize i = 0; i < n; i += 2 * w) {
            cx_usize m = i + w < n ? i + w : n, e = i + 2 * w < n ? i + 2 * w : n;
            if (e - m == w) CX_SORT_F(_merge_both)(from + i, w, to + i, at);
            else CX_SORT_F(_merge_into)(from + i, from + m, from + m, from + e, to + i);
        }
        CX_SORT_T* t = from;
        from = to;
        to = t;
    }
    if (from != v) __builtin_memcpy(v, from, n * sizeof(CX_SORT_T));
}

/* Every value in v[0..n) is at least `*floor`, the pivot that set the run apart, if there is one.
 * `limit` is how many more partitions may go before merge sort takes over. */
static void CX_SORT_F(_stable_quick)(CX_SORT_T* v, cx_usize n, CX_SORT_T* buf, const CX_SORT_T* floor, int limit,
                                      const char* at) {
    CX_SORT_T below;  // the pivot the rest of the run comes after, copied, since elements move
    for (;;) {
        if (n <= 32) {
            CX_SORT_F(_small_stable)(v, n, buf, at);
            return;
        }
        if (limit == 0) {
            CX_SORT_F(_merge_sort)(v, n, buf);
            return;
        }
        limit--;
        CX_SORT_T pivot = v[n >= 512 ? CX_SORT_F(_median_far)(v, 0, n / 8) : CX_SORT_F(_pivot)(v, n)];
        if (floor && !CX_SORT_LESS(floor, &pivot)) {
            // The pivot equals the floor, so every value not greater than it equals it: set them
            // all aside, and sort what's greater.
            cx_usize same = CX_SORT_F(_split)(v, n, buf, &pivot, 1);
            v += same;
            n -= same;
            floor = 0;
            continue;
        }
        // At least the pivot goes right; if nothing goes left, the next pivot finds its copies.
        cx_usize mid = CX_SORT_F(_split)(v, n, buf, &pivot, 0);
        CX_SORT_F(_stable_quick)(v, mid, buf, floor, limit, at);
        below = pivot;
        floor = &below;
        v += mid;
        n -= mid;
    }
}
#endif

#ifdef CX_SORT_STATIC
static
#endif
#ifdef CX_SORT_STABLE
void CX_SORT_NAME(CX_SORT_T* v, cx_usize n, const char* at) {
#else
void CX_SORT_NAME(CX_SORT_T* v, cx_usize n) {
#endif
    if (n < 2) return;
#if defined(CX_SORT_NETWORK) && !defined(CX_SORT_STABLE)
    // A network takes as long on a list in order as on any other.
    if (n < 32) {
        CX_SORT_F(_tiny)(v, n);
        return;
    }
#endif
    // A list in order, or strictly in reverse. The elements are compared 16 at a time with no
    // branch between, so the compiler can compare several at once.
    int down = CX_SORT_LESS(&v[1], &v[0]);
#ifndef CX_SORT_STABLE
    if (down) {
        if (!CX_SORT_F(_reversed)(v, n)) CX_SORT_F(_core)(v, n, v + n);
        return;
    }
#endif
    cx_usize i = 2;
    for (; i + 16 <= n; i += 16) {
        int wrong = 0;
        for (int j = 0; j < 16; j++) wrong |= CX_SORT_LESS(&v[i + j], &v[i + j - 1]) != down;
        if (wrong) break;
    }
    while (i < n && CX_SORT_LESS(&v[i], &v[i - 1]) == down) i++;
    if (i == n) {
        if (down) {
            for (cx_usize l = 0, r = n - 1; l < r; l++, r--) CX_SORT_SWAP(v[l], v[r]);
        }
        return;
    }
#ifdef CX_SORT_STABLE
    if (n <= 20) {
        CX_SORT_F(_insertion)(v, n);
        return;
    }
    cx_usize bytes = n * sizeof(CX_SORT_T);
    CX_SORT_T* buf = (CX_SORT_T*)cx_alloc(bytes);
    CX_SORT_F(_stable_quick)(v, n, buf, 0, 2 * (63 - __builtin_clzll((unsigned long long)n)), at);
    cx_free(buf, bytes);
#else
    CX_SORT_F(_core)(v, n, v + n);
#endif
}

#undef CX_SORT_SWAP
#undef CX_SORT_CAS
#undef CX_SORT_F
#undef CX_SORT_CAT
#undef CX_SORT_CAT2
#undef CX_SORT_T
#undef CX_SORT_NAME
#undef CX_SORT_LESS
#undef CX_SORT_KEY
#undef CX_SORT_KEY_T
#undef CX_SORT_UNKEY
#undef CX_SORT_STATIC
#undef CX_SORT_NETWORK
#undef CX_SORT_STABLE
