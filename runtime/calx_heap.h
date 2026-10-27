/* `PriorityQueue<T>`, as a template (D113). Define
 *
 *   CX_HP_T            the queue's type: a list's struct, of `ptr`, `len` and `cap`
 *   CX_HP_NAME         the start of its functions' names, as `cx_heap_i32`
 *   CX_HP_E            the element type
 *   CX_HP_LESS(a, b)   whether the element `a` comes before `b`. Name the parameters so no field
 *                      shares their names, as `cx_ha`
 *   CX_HP_DROP(p)      optional: free what the element `p` points to owns
 *   CX_HP_CHEAP        optional: CX_HP_LESS is a compare of numbers with no branch, so one
 *                      more costs less than waiting on memory
 *
 * and include this file, once for each queue type. It undefines them all.
 *
 * It's a binary heap in an array, least first, as Java's `PriorityQueue` is: the children of
 * place `k` are at `2k + 1` and `2k + 2`, and no element comes before its parent. Every change
 * moves the elements as Java's does, so after the same changes the array holds the same elements
 * in the same places, and a loop, which goes along the array, goes in the order of Java's
 * iterator:
 *
 * - an element going up passes only parents it comes before, so it stops below an equal one;
 * - an element going down goes to the right child only when that comes before the left, and
 *   passes only children that come before it (`down` gets there another way);
 * - taking out place `i` puts the last element there, down, or up if it can't go down.
 */

#define CX_HP_CAT2(a, b) a##b
#define CX_HP_CAT(a, b) CX_HP_CAT2(a, b)
#define CX_HP_F(x) CX_HP_CAT(CX_HP_NAME, CX_HP_CAT(_, x))
#ifndef CX_HP_DROP
#define CX_HP_DROP(p) ((void)0)
#endif

/* Puts `x` at place `k`, or above it, as far up as it goes; where it ends up. */
CX_INLINE cx_usize CX_HP_F(up)(CX_HP_E* es, cx_usize k, CX_HP_E x) {
    while (k > 0) {
        cx_usize p = (k - 1) >> 1;
        if (!CX_HP_LESS(x, es[p])) break;
        es[k] = es[p];
        k = p;
    }
    es[k] = x;
    return k;
}

#ifdef CX_HP_CHEAP
/* Moves the hole at `k` down the picked children as `down` does, two levels at a time, while
   both have children: where it ends up. Each level waits on the last, since which child is
   picked says where the next two are; this loads the two children and all four grandchildren at
   once, pairs the grandchildren as the children are, and lets the picks choose among them, so
   one wait on memory covers two levels, for a third compare more. With `fetch` it also asks for
   the next two levels' lines as it goes, for a queue too big for the second cache. */
CX_INLINE cx_usize CX_HP_F(down2)(CX_HP_E* es, cx_usize k, cx_usize n, cx_bool fetch) {
    while (4 * k + 6 < n) {
        cx_usize c = 2 * k + 1, g = 2 * c + 1;
        if (fetch) {
            /* Addresses as numbers: a line past the end is asked for, never read. */
            __UINTPTR_TYPE__ base = (__UINTPTR_TYPE__)es;
            __builtin_prefetch((const void*)(base + (2 * g + 1) * sizeof(CX_HP_E)));
            __builtin_prefetch((const void*)(base + (4 * g + 3) * sizeof(CX_HP_E)));
            __builtin_prefetch((const void*)(base + (4 * g + 11) * sizeof(CX_HP_E)));
        }
        cx_usize p = CX_HP_LESS(es[c + 1], es[c]);
        cx_usize p0 = CX_HP_LESS(es[g + 1], es[g]), p1 = CX_HP_LESS(es[g + 3], es[g + 2]);
        cx_usize m = c + p, gm = g + 2 * p + (p ? p1 : p0);
        /* By place, not by value: choosing between two values of 16 bytes takes more than
           loading the one picked again, which is in the first cache by now. */
        es[k] = es[m];
        es[m] = es[gm];
        k = gm;
    }
    return k;
}
#endif

/* Puts `x` at place `k` of the first `n`, or below it, as far down as it goes; where it ends up.
   Java's goes down a child at a time, comparing the two children and then the one it picks with
   `x`, and stops at the first that `x` doesn't come after. This moves the hole down along the
   picked children to the bottom first, a compare a level and no branch on it, then brings `x` back
   up past those it doesn't come after. The picked children never decrease, so `x` ends up past
   just the ones before it, as in Java's, and the array is the same; but `x` is the last element,
   which usually belongs near the bottom, so it takes about half the compares, as Rust's does. */
CX_INLINE cx_usize CX_HP_F(down)(CX_HP_E* es, cx_usize k, CX_HP_E x, cx_usize n) {
    cx_usize top = k, c;
#ifdef CX_HP_CHEAP
    if (n * sizeof(CX_HP_E) > (1u << 18)) k = CX_HP_F(down2)(es, k, n, 1);
    k = CX_HP_F(down2)(es, k, n, 0);
#endif
    while ((c = 2 * k + 1) + 1 < n) {
        c += CX_HP_LESS(es[c + 1], es[c]);
        es[k] = es[c];
        k = c;
    }
    if (c + 1 == n) {
        es[k] = es[c];
        k = c;
    }
    while (k > top) {
        cx_usize p = (k - 1) >> 1;
        if (CX_HP_LESS(es[p], x)) break;
        es[k] = es[p];
        k = p;
    }
    es[k] = x;
    return k;
}

static void CX_HP_F(push)(CX_HP_T* q, CX_HP_E x) {
    if (q->len == q->cap) q->ptr = (CX_HP_E*)cx_grow(q->ptr, &q->cap, q->len, 1, sizeof(CX_HP_E));
    cx_usize k = q->len++;
    (void)CX_HP_F(up)(q->ptr, k, x);
}

/* Moves the least element to `*out`; whether there was one. */
static cx_bool CX_HP_F(pop)(CX_HP_T* q, CX_HP_E* out) {
    if (!q->len) return 0;
    *out = q->ptr[0];
    cx_usize n = --q->len;
    if (n) (void)CX_HP_F(down)(q->ptr, 0, q->ptr[n], n);
    return 1;
}

/* Takes place `i` out, once its element is freed or moved out: the last element goes there, and
   down, or else up. Whether it went up, so it's now before `i`, where a loop has been. */
static cx_bool CX_HP_F(take)(CX_HP_T* q, cx_usize i) {
    cx_usize s = --q->len;
    if (s == i) return 0;
    CX_HP_E x = q->ptr[s];
    if (CX_HP_F(down)(q->ptr, i, x, s) != i) return 0;
    return CX_HP_F(up)(q->ptr, i, x) != i;
}

/* The same moves as `up` and `down`, carrying each element's tag with it. */
static cx_usize CX_HP_F(tag_up)(CX_HP_E* es, cx_usize* tags, cx_usize k, CX_HP_E x, cx_usize t) {
    while (k > 0) {
        cx_usize p = (k - 1) >> 1;
        if (!CX_HP_LESS(x, es[p])) break;
        es[k] = es[p];
        tags[k] = tags[p];
        k = p;
    }
    es[k] = x;
    tags[k] = t;
    return k;
}

static cx_usize CX_HP_F(tag_down)(CX_HP_E* es, cx_usize* tags, cx_usize k, CX_HP_E x, cx_usize t, cx_usize n) {
    cx_usize half = n >> 1;
    while (k < half) {
        cx_usize c = 2 * k + 1;
        if (c + 1 < n && CX_HP_LESS(es[c + 1], es[c])) c++;
        if (!CX_HP_LESS(es[c], x)) break;
        es[k] = es[c];
        tags[k] = tags[c];
        k = c;
    }
    es[k] = x;
    tags[k] = t;
    return k;
}

/* `take`, carrying the tags: whether the last element went up, before `i`. */
static cx_bool CX_HP_F(tag_take)(CX_HP_T* q, cx_usize* tags, cx_usize i) {
    cx_usize s = --q->len;
    if (s == i) return 0;
    CX_HP_E x = q->ptr[s];
    cx_usize t = tags[s];
    if (CX_HP_F(tag_down)(q->ptr, tags, i, x, t, s) != i) return 0;
    return CX_HP_F(tag_up)(q->ptr, tags, i, x, t) != i;
}

/* Takes out each element whose mark is set, `marks` being one for each place in the order a loop
   goes, as a Java loop over the queue that calls its iterator's `remove` on them does. The loop
   goes along the array. Taking out place `i` moves the last element there: if it goes down, or
   stays, the loop looks at place `i` again, since what's there is new to it; if it goes up, past
   the loop, the loop looks at it after the end of the array, with any others that did, in the
   order they went up. Those are found again by looking for them, so they're taken out with the
   moves that finding makes. Each element is tagged with its first place to tell which mark is
   its. */
static void CX_HP_F(remove_marked)(CX_HP_T* q, const cx_bool* marks, cx_usize n, const char* at) {
    if (__builtin_expect(n != q->len, 0)) cx_panic_marks(n, q->len, at);
    if (!n) return;
    cx_usize* tags = (cx_usize*)cx_alloc(n * sizeof(cx_usize));
    cx_usize* later = (cx_usize*)cx_alloc(n * sizeof(cx_usize));
    cx_usize first = 0, end = 0;
    for (cx_usize i = 0; i < n; i++) tags[i] = i;
    cx_usize cursor = 0;
    while (cursor < q->len) {
        cx_usize i = cursor++;
        if (!marks[tags[i]]) continue;
        CX_HP_DROP(&q->ptr[i]);
        cx_usize last = tags[q->len - 1];
        if (CX_HP_F(tag_take)(q, tags, i)) later[end++] = last;
        else cursor--;
    }
    while (first < end) {
        cx_usize t = later[first++];
        if (!marks[t]) continue;
        for (cx_usize j = 0; j < q->len; j++) {
            if (tags[j] != t) continue;
            CX_HP_DROP(&q->ptr[j]);
            (void)CX_HP_F(tag_take)(q, tags, j);
            break;
        }
    }
    cx_free(tags, n * sizeof(cx_usize));
    cx_free(later, n * sizeof(cx_usize));
}

#undef CX_HP_CAT2
#undef CX_HP_CAT
#undef CX_HP_F
#undef CX_HP_T
#undef CX_HP_NAME
#undef CX_HP_E
#undef CX_HP_LESS
#undef CX_HP_DROP
#undef CX_HP_CHEAP
