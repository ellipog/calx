/* `xs.sort()`, as a template (D96). Define
 *
 *   CX_SORT_T           the element type
 *   CX_SORT_NAME        the function's name, which sorts `CX_SORT_T* v` of length `cx_usize n`
 *   CX_SORT_LESS(a, b)  whether the element `a` points to comes before the one `b` points to.
 *                       Name the parameters so no field shares their names, as `cx_sa`
 *   CX_SORT_KEY(x)      optional: an unsigned integer that orders as the element `x` does, of
 *   CX_SORT_KEY_T       the key's type, the size of the element, and
 *   CX_SORT_UNKEY(k)    the element whose key is `k`. Long lists are then radix sorted
 *   CX_SORT_NETWORK     optional: sort short runs with a sorting network, for integers
 *   CX_SORT_STATIC      optional: make the function `static`
 *
 * and include this file, once for each element type. It undefines them all.
 *
 * Equal elements may change places: an `Order` that's derived compares everything a value holds,
 * so two equal elements are the same value, and no program can tell them apart. That leaves the
 * fastest algorithms free to use:
 *
 * - A list already in order, or in reverse order, is found in one pass and done.
 * - Integers in long lists are radix sorted, a byte at a time from the lowest, skipping each byte
 *   that's the same in every element. That's a pass over the list per byte, with no comparisons.
 * - Everything else is quicksorted. The pivot is the median of 3 elements, or of 3 medians of 3
 *   in longer runs. The partition has no branches on the comparison, so a mispredicted branch
 *   doesn't cost a pipeline flush on every other element. When a pivot equals the one the run's
 *   values all come after, the run holds many copies of it, and they're set aside in one pass,
 *   so lists with few distinct values take linear time. Runs of up to 16 are insertion sorted,
 *   or for integers sorted by a network, and a run that's been split too unevenly too often is
 *   heapsorted, so the worst case is O(n log n).
 *
 * Moving an element copies its bytes, as every move in Calx does, so nothing is copied deeply,
 * dropped or freed. The only memory used is the radix sort's buffer, the size of the list. */

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

#ifdef CX_SORT_NETWORK
/* Sorts up to 16 elements with no branch on a comparison: runs of 4 by a sorting network, then
 * runs merged in pairs through a buffer, taking from one side or the other by choosing a
 * pointer. For integers that's faster than insertion, whose every element costs a mispredicted
 * branch; for elements that take several comparisons, it's slower. */
static inline void CX_SORT_F(_order2)(CX_SORT_T* x, CX_SORT_T* y) {
    int c = CX_SORT_LESS(y, x);
    CX_SORT_T lo = *(c ? y : x), hi = *(c ? x : y);
    *x = lo;
    *y = hi;
}

static void CX_SORT_F(_merge)(const CX_SORT_T* l, const CX_SORT_T* le, const CX_SORT_T* r, const CX_SORT_T* re, CX_SORT_T* d) {
    while (l < le && r < re) {
        int c = CX_SORT_LESS(r, l);
        *d++ = *(c ? r : l);
        r += c;
        l += !c;
    }
    while (l < le) *d++ = *l++;
    while (r < re) *d++ = *r++;
}

static void CX_SORT_F(_small)(CX_SORT_T* v, cx_usize n) {
    if (n < 8) {
        CX_SORT_F(_insertion)(v, n);
        return;
    }
    CX_SORT_T buf[16];
    cx_usize i = 0;
    for (; i + 4 <= n; i += 4) {
        CX_SORT_F(_order2)(&v[i], &v[i + 1]);
        CX_SORT_F(_order2)(&v[i + 2], &v[i + 3]);
        CX_SORT_F(_order2)(&v[i], &v[i + 2]);
        CX_SORT_F(_order2)(&v[i + 1], &v[i + 3]);
        CX_SORT_F(_order2)(&v[i + 1], &v[i + 2]);
    }
    CX_SORT_F(_insertion)(v + i, n - i);
    CX_SORT_T *from = v, *to = buf;
    for (cx_usize w = 4; w < n; w *= 2) {
        for (cx_usize s = 0; s < n; s += 2 * w) {
            cx_usize m = s + w < n ? s + w : n, e = s + 2 * w < n ? s + 2 * w : n;
            CX_SORT_F(_merge)(from + s, from + m, from + m, from + e, to + s);
        }
        CX_SORT_T* t = from;
        from = to;
        to = t;
    }
    if (from != v) __builtin_memcpy(v, from, n * sizeof(CX_SORT_T));
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
 * `limit` is how many more partitions may go before heapsort takes over. */
static void CX_SORT_F(_quick)(CX_SORT_T* v, cx_usize n, const CX_SORT_T* floor, int limit) {
    for (;;) {
        if (n <= 16) {
#ifdef CX_SORT_NETWORK
            CX_SORT_F(_small)(v, n);
#else
            CX_SORT_F(_insertion)(v, n);
#endif
            return;
        }
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
        CX_SORT_F(_quick)(v, mid, floor, limit);
        floor = &v[mid];
        v += mid + 1;
        n -= mid + 1;
    }
}

#ifdef CX_SORT_KEY
/* Radix sorts v[0..n) and says so, unless a quicksort would be faster. One pass finds the bytes
 * that differ between elements, which says which is faster, and the next counts every byte. Then
 * each pass moves the elements by one of the bytes that differ. With only one, the counts are all
 * it takes: each value is written as often as it was counted, in place. */
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

#ifdef CX_SORT_STATIC
static
#endif
void CX_SORT_NAME(CX_SORT_T* v, cx_usize n) {
    if (n < 2) return;
    // A list in order, or strictly in reverse. The elements are compared 16 at a time with no
    // branch between, so the compiler can compare several at once.
    int down = CX_SORT_LESS(&v[1], &v[0]);
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
#ifdef CX_SORT_KEY
    if (n >= 128 && CX_SORT_F(_radix)(v, n)) return;
#endif
    CX_SORT_F(_quick)(v, n, 0, 2 * (63 - __builtin_clzll((unsigned long long)n)));
}

#undef CX_SORT_SWAP
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
