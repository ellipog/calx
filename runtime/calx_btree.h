/* `SortedMap<K, V>` and `SortedSet<T>`, as a template (D112). Define
 *
 *   CX_BT_T            the table's type: a struct of `void* root`, `void* first`, `void* last`,
 *                      `cx_usize len` and `cx_u32 height`
 *   CX_BT_NAME         the start of its functions' names, as `cx_smap_i64_i32`
 *   CX_BT_K            the key type
 *   CX_BT_V            the value type, for a map; a set leaves it undefined
 *   CX_BT_KL           what a key is looked up as: `cx_str` for a `String`, else CX_BT_K
 *   CX_BT_LOOK(k)      the key `k`, a place, as a CX_BT_KL
 *   CX_BT_LESS(a, b)   whether the CX_BT_KL `a` comes before `b`. Name the parameters so no field
 *                      shares their names, as `cx_la`
 *   CX_BT_SCALAR       optional: CX_BT_LESS is `<` on numbers, so a node is searched by counting
 *   CX_BT_DROP_K(p), CX_BT_DROP_V(p)  optional: free what the key or value `p` points to owns
 *   CX_BT_COPY_K(p), CX_BT_COPY_V(p)  optional: a copy of it that owns memory of its own
 *
 * and include this file, once for each table type. It undefines them all.
 *
 * It's a B+ tree. The leaves hold every key, in order, each leaf's keys in one array and their
 * values in another, and are linked both ways, so a loop or a range is a walk along them. An inner
 * node holds the children and, between each two, a separator: the first key of the right one's
 * leaves when it was split off, so every key under a child is at least the separator before it
 * and less than the one after. A key that owns memory is copied into its separators, which can
 * outlive it. A node is searched by counting the keys before the one looked for, which AVX2 does
 * 4 at a time for numbers, or else by a binary search without branches.
 *
 * A full node splits in two halves, except a leaf that's last in the tree, when the new key goes
 * last in it: that one keeps all its keys and the new one starts the next, as do the inner nodes
 * above it, so keys put in in order fill every node. A leaf or inner node left with fewer than
 * half takes one from a neighbour that has more than half, or else is merged with it.
 */

#define CX_BT_CAT2(a, b) a##b
#define CX_BT_CAT(a, b) CX_BT_CAT2(a, b)
#define CX_BT_F(x) CX_BT_CAT(CX_BT_NAME, CX_BT_CAT(_, x))
#define CX_BT_LEAF CX_BT_F(leaf)
#define CX_BT_INNER CX_BT_F(inner)

#ifdef CX_BT_V
#define CX_BT_VSIZE sizeof(CX_BT_V)
#define CX_BT_WITH_V(...) __VA_ARGS__
#else
#define CX_BT_VSIZE 0
#define CX_BT_WITH_V(...)
#endif
#ifndef CX_BT_DROP_K
#define CX_BT_DROP_K(p) ((void)0)
#endif
#ifndef CX_BT_DROP_V
#define CX_BT_DROP_V(p) ((void)0)
#endif
/* A separator: a key's own copy, when it owns memory. */
#ifdef CX_BT_COPY_K
#define CX_BT_SEP(p) CX_BT_COPY_K(p)
#define CX_BT_DROP_SEP(p) CX_BT_DROP_K(p)
#else
#define CX_BT_SEP(p) (*(p))
#define CX_BT_DROP_SEP(p) ((void)0)
#endif

/* Nodes of about 1 KB: leaves of 128 keys and values of 8 bytes down to 8 of more than 64, and
   inner nodes of 64 children down to 16. Measured on 1M keys of `i64` to `i64`: 32 a leaf made
   random puts and removes 10 to 20% slower, and loops and ranges 2 times, and 128 didn't pay for
   moving 2 KB on each `pop_first`. */
#define CX_BT_ESIZE (sizeof(CX_BT_K) + CX_BT_VSIZE)
enum {
    CX_BT_F(lcap) = CX_BT_ESIZE <= 8 ? 128 : CX_BT_ESIZE <= 16 ? 64 : CX_BT_ESIZE <= 32 ? 32 : CX_BT_ESIZE <= 64 ? 16 : 8,
    CX_BT_F(icap) = sizeof(CX_BT_K) <= 8 ? 63 : sizeof(CX_BT_K) <= 16 ? 31 : 15,
};
#define CX_BT_LCAP ((cx_u32)CX_BT_F(lcap))
#define CX_BT_ICAP ((cx_u32)CX_BT_F(icap))
#define CX_BT_LMIN (CX_BT_LCAP / 2)
#define CX_BT_IMIN (CX_BT_ICAP / 2)
/* The most inner nodes above a leaf: every inner node but the last of its level has at least
   CX_BT_IMIN + 1 children, so 48 levels is far past any table that fits in memory. */
#define CX_BT_DEPTH 48

typedef struct CX_BT_LEAF CX_BT_LEAF;
struct CX_BT_LEAF {
    CX_BT_LEAF* prev;
    CX_BT_LEAF* next;
    cx_u32 n;
    CX_BT_K k[CX_BT_F(lcap)];
#ifdef CX_BT_V
    CX_BT_V v[CX_BT_F(lcap)];
#endif
};
typedef struct CX_BT_INNER {
    cx_u32 n; /* separators; there's one more child */
    CX_BT_K k[CX_BT_F(icap)];
    void* c[CX_BT_F(icap) + 1];
} CX_BT_INNER;

/* How many of the `n` keys at `k` come before `x`. */
CX_INLINE cx_u32 CX_BT_F(lower)(const CX_BT_K* k, cx_u32 n, CX_BT_KL x) {
#ifdef CX_BT_SCALAR
    cx_u32 c = 0;
    for (cx_u32 i = 0; i < n; i++) c += CX_BT_LESS(CX_BT_LOOK(k[i]), x);
    return c;
#else
    if (!n) return 0;
    const CX_BT_K* base = k;
    while (n > 1) {
        cx_u32 half = n / 2;
        base = CX_BT_LESS(CX_BT_LOOK(base[half]), x) ? base + half : base;
        n -= half;
    }
    return (cx_u32)(base - k) + (cx_u32)CX_BT_LESS(CX_BT_LOOK(*base), x);
#endif
}

/* How many of them don't come after `x`. */
CX_INLINE cx_u32 CX_BT_F(upper)(const CX_BT_K* k, cx_u32 n, CX_BT_KL x) {
#ifdef CX_BT_SCALAR
    cx_u32 c = 0;
    for (cx_u32 i = 0; i < n; i++) c += !CX_BT_LESS(x, CX_BT_LOOK(k[i]));
    return c;
#else
    if (!n) return 0;
    const CX_BT_K* base = k;
    while (n > 1) {
        cx_u32 half = n / 2;
        base = !CX_BT_LESS(x, CX_BT_LOOK(base[half])) ? base + half : base;
        n -= half;
    }
    return (cx_u32)(base - k) + (cx_u32)!CX_BT_LESS(x, CX_BT_LOOK(*base));
#endif
}

/* The leaf `x` belongs in, and in `*at`, how many keys there come before it; null when the tree
   is empty. */
CX_INLINE CX_BT_LEAF* CX_BT_F(seek)(const CX_BT_T* t, CX_BT_KL x, cx_u32* at) {
    void* p = t->root;
    for (cx_u32 h = t->height; h; h--) {
        const CX_BT_INNER* in = (const CX_BT_INNER*)p;
        p = in->c[CX_BT_F(upper)(in->k, in->n, x)];
    }
    CX_BT_LEAF* l = (CX_BT_LEAF*)p;
    *at = l ? CX_BT_F(lower)(l->k, l->n, x) : 0;
    return l;
}

/* The leaf holding the key `x`, with its place in `*at`, or null. */
CX_INLINE CX_BT_LEAF* CX_BT_F(find)(const CX_BT_T* t, CX_BT_KL x, cx_u32* at) {
    CX_BT_LEAF* l = CX_BT_F(seek)(t, x, at);
    if (!l || *at == l->n || CX_BT_LESS(x, CX_BT_LOOK(l->k[*at]))) return 0;
    return l;
}

#ifdef CX_BT_V
/* The value of the key `x`, or null. */
CX_INLINE CX_BT_V* CX_BT_F(at)(const CX_BT_T* t, CX_BT_KL x) {
    cx_u32 i;
    CX_BT_LEAF* l = CX_BT_F(find)(t, x, &i);
    return l ? &l->v[i] : 0;
}
#endif

CX_INLINE cx_bool CX_BT_F(has)(const CX_BT_T* t, CX_BT_KL x) {
    cx_u32 i;
    return CX_BT_F(find)(t, x, &i) != 0;
}

/* The key nearest `x` on one side: the first that isn't before it (0), the first after it (1),
   the last that isn't after it (2), or the last before it (3); null if there's none. */
static inline const CX_BT_K* CX_BT_F(near)(const CX_BT_T* t, CX_BT_KL x, int how) {
    void* p = t->root;
    if (!p) return 0;
    for (cx_u32 h = t->height; h; h--) {
        const CX_BT_INNER* in = (const CX_BT_INNER*)p;
        p = in->c[CX_BT_F(upper)(in->k, in->n, x)];
    }
    const CX_BT_LEAF* l = (const CX_BT_LEAF*)p;
    cx_u32 i = how == 0 || how == 3 ? CX_BT_F(lower)(l->k, l->n, x) : CX_BT_F(upper)(l->k, l->n, x);
    if (how < 2) {
        if (i < l->n) return &l->k[i];
        return l->next ? &l->next->k[0] : 0;
    }
    if (i) return &l->k[i - 1];
    return l->prev ? &l->prev->k[l->prev->n - 1] : 0;
}

static CX_BT_LEAF* CX_BT_F(new_leaf)(void) {
    CX_BT_LEAF* l = (CX_BT_LEAF*)cx_alloc(sizeof(CX_BT_LEAF));
    l->prev = l->next = 0;
    l->n = 0;
    return l;
}

static CX_BT_INNER* CX_BT_F(new_inner)(void) {
    CX_BT_INNER* in = (CX_BT_INNER*)cx_alloc(sizeof(CX_BT_INNER));
    in->n = 0;
    return in;
}

/* Puts the key `k` with its value at place `i` of a leaf that has room. */
CX_INLINE void CX_BT_F(leaf_put)(CX_BT_LEAF* l, cx_u32 i, CX_BT_K k CX_BT_WITH_V(, CX_BT_V v)) {
    __builtin_memmove(l->k + i + 1, l->k + i, (l->n - i) * sizeof(CX_BT_K));
    l->k[i] = k;
    CX_BT_WITH_V(__builtin_memmove(l->v + i + 1, l->v + i, (l->n - i) * sizeof(CX_BT_V)); l->v[i] = v;)
    l->n++;
}

/* Puts `k` into the table, with `v`. A key that's there keeps its place, and the new key is
   freed; the value is replaced, and the old one freed. */
static void CX_BT_F(put)(CX_BT_T* t, CX_BT_K k CX_BT_WITH_V(, CX_BT_V v)) {
    if (!t->root) {
        CX_BT_LEAF* l = CX_BT_F(new_leaf)();
        l->n = 1;
        l->k[0] = k;
        CX_BT_WITH_V(l->v[0] = v;)
        t->root = t->first = t->last = l;
        t->height = 0;
        t->len = 1;
        return;
    }
    CX_BT_KL x = CX_BT_LOOK(k);
    CX_BT_INNER* path[CX_BT_DEPTH];
    cx_u32 at[CX_BT_DEPTH];
    void* p = t->root;
    for (cx_u32 h = 0; h < t->height; h++) {
        CX_BT_INNER* in = (CX_BT_INNER*)p;
        cx_u32 i = CX_BT_F(upper)(in->k, in->n, x);
        path[h] = in;
        at[h] = i;
        p = in->c[i];
    }
    CX_BT_LEAF* l = (CX_BT_LEAF*)p;
    cx_u32 i = CX_BT_F(lower)(l->k, l->n, x);
    if (i < l->n && !CX_BT_LESS(x, CX_BT_LOOK(l->k[i]))) {
        CX_BT_DROP_K(&k);
        CX_BT_WITH_V(CX_BT_DROP_V(&l->v[i]); l->v[i] = v;)
        return;
    }
    t->len++;
    if (l->n < CX_BT_LCAP) {
        CX_BT_F(leaf_put)(l, i, k CX_BT_WITH_V(, v));
        return;
    }
    /* Full: of the CX_BT_LCAP + 1 keys, the first `keep` stay, and the rest go to a new leaf. */
    cx_bool append = l == t->last && i == CX_BT_LCAP;
    cx_u32 keep = append ? CX_BT_LCAP : (CX_BT_LCAP + 1) / 2;
    CX_BT_LEAF* r = CX_BT_F(new_leaf)();
    if (i < keep) {
        cx_u32 m = CX_BT_LCAP - (keep - 1);
        __builtin_memcpy(r->k, l->k + keep - 1, m * sizeof(CX_BT_K));
        CX_BT_WITH_V(__builtin_memcpy(r->v, l->v + keep - 1, m * sizeof(CX_BT_V));)
        r->n = m;
        l->n = keep - 1;
        CX_BT_F(leaf_put)(l, i, k CX_BT_WITH_V(, v));
    } else {
        cx_u32 j = i - keep;
        __builtin_memcpy(r->k, l->k + keep, j * sizeof(CX_BT_K));
        r->k[j] = k;
        __builtin_memcpy(r->k + j + 1, l->k + i, (CX_BT_LCAP - i) * sizeof(CX_BT_K));
        CX_BT_WITH_V(__builtin_memcpy(r->v, l->v + keep, j * sizeof(CX_BT_V)); r->v[j] = v;
                     __builtin_memcpy(r->v + j + 1, l->v + i, (CX_BT_LCAP - i) * sizeof(CX_BT_V));)
        r->n = CX_BT_LCAP - keep + 1;
        l->n = keep;
    }
    r->prev = l;
    r->next = l->next;
    if (l->next) l->next->prev = r;
    else t->last = r;
    l->next = r;
    /* The new leaf's separator and pointer go into the parent, which splits in turn if it's full. */
    CX_BT_K sep = CX_BT_SEP(&r->k[0]);
    void* child = r;
    for (cx_u32 h = t->height; h-- > 0;) {
        CX_BT_INNER* in = path[h];
        cx_u32 j = at[h];
        if (in->n < CX_BT_ICAP) {
            __builtin_memmove(in->k + j + 1, in->k + j, (in->n - j) * sizeof(CX_BT_K));
            __builtin_memmove(in->c + j + 2, in->c + j + 1, (in->n - j) * sizeof(void*));
            in->k[j] = sep;
            in->c[j + 1] = child;
            in->n++;
            return;
        }
        CX_BT_K ks[CX_BT_F(icap) + 1];
        void* cs[CX_BT_F(icap) + 2];
        __builtin_memcpy(ks, in->k, j * sizeof(CX_BT_K));
        ks[j] = sep;
        __builtin_memcpy(ks + j + 1, in->k + j, (CX_BT_ICAP - j) * sizeof(CX_BT_K));
        __builtin_memcpy(cs, in->c, (j + 1) * sizeof(void*));
        cs[j + 1] = child;
        __builtin_memcpy(cs + j + 2, in->c + j + 1, (CX_BT_ICAP - j) * sizeof(void*));
        /* The first `m` separators stay, the next goes up, and the rest go to a new node. */
        cx_u32 m = append ? CX_BT_ICAP - 1 : (CX_BT_ICAP + 1) / 2;
        CX_BT_INNER* s = CX_BT_F(new_inner)();
        __builtin_memcpy(in->k, ks, m * sizeof(CX_BT_K));
        __builtin_memcpy(in->c, cs, (m + 1) * sizeof(void*));
        in->n = m;
        s->n = CX_BT_ICAP - m;
        __builtin_memcpy(s->k, ks + m + 1, s->n * sizeof(CX_BT_K));
        __builtin_memcpy(s->c, cs + m + 1, (s->n + 1) * sizeof(void*));
        sep = ks[m];
        child = s;
    }
    CX_BT_INNER* root = CX_BT_F(new_inner)();
    root->n = 1;
    root->k[0] = sep;
    root->c[0] = t->root;
    root->c[1] = child;
    t->root = root;
    t->height++;
}

/* Takes separator `j` and the child after it out of `in`, without freeing either. */
CX_INLINE void CX_BT_F(unhook)(CX_BT_INNER* in, cx_u32 j) {
    __builtin_memmove(in->k + j, in->k + j + 1, (in->n - j - 1) * sizeof(CX_BT_K));
    __builtin_memmove(in->c + j + 1, in->c + j + 2, (in->n - j - 1) * sizeof(void*));
    in->n--;
}

/* Appends leaf `b` to leaf `a`, and frees `b`. */
static void CX_BT_F(merge_leaves)(CX_BT_T* t, CX_BT_LEAF* a, CX_BT_LEAF* b) {
    __builtin_memcpy(a->k + a->n, b->k, b->n * sizeof(CX_BT_K));
    CX_BT_WITH_V(__builtin_memcpy(a->v + a->n, b->v, b->n * sizeof(CX_BT_V));)
    a->n += b->n;
    a->next = b->next;
    if (b->next) b->next->prev = a;
    else t->last = a;
    cx_free(b, sizeof(CX_BT_LEAF));
}

/* After an inner node at depth `h` on the path lost a child: if it has too few, it takes one
   from a neighbour or is merged with one, and so on up; a root left with one child gives way to
   it. */
static void CX_BT_F(fix_inner)(CX_BT_T* t, CX_BT_INNER** path, const cx_u32* at, cx_u32 h) {
    for (;;) {
        CX_BT_INNER* in = path[h];
        if (!h) {
            if (!in->n) {
                t->root = in->c[0];
                t->height--;
                cx_free(in, sizeof(CX_BT_INNER));
            }
            return;
        }
        if (in->n >= CX_BT_IMIN) return;
        CX_BT_INNER* g = path[h - 1];
        cx_u32 j = at[h - 1];
        CX_BT_INNER* left = j ? (CX_BT_INNER*)g->c[j - 1] : 0;
        CX_BT_INNER* right = j < g->n ? (CX_BT_INNER*)g->c[j + 1] : 0;
        if (left && left->n > CX_BT_IMIN) {
            __builtin_memmove(in->k + 1, in->k, in->n * sizeof(CX_BT_K));
            __builtin_memmove(in->c + 1, in->c, (in->n + 1) * sizeof(void*));
            in->k[0] = g->k[j - 1];
            in->c[0] = left->c[left->n];
            in->n++;
            g->k[j - 1] = left->k[left->n - 1];
            left->n--;
            return;
        }
        if (right && right->n > CX_BT_IMIN) {
            in->k[in->n] = g->k[j];
            in->c[in->n + 1] = right->c[0];
            in->n++;
            g->k[j] = right->k[0];
            __builtin_memmove(right->k, right->k + 1, (right->n - 1) * sizeof(CX_BT_K));
            __builtin_memmove(right->c, right->c + 1, right->n * sizeof(void*));
            right->n--;
            return;
        }
        if (left) {
            left->k[left->n] = g->k[j - 1];
            __builtin_memcpy(left->k + left->n + 1, in->k, in->n * sizeof(CX_BT_K));
            __builtin_memcpy(left->c + left->n + 1, in->c, (in->n + 1) * sizeof(void*));
            left->n += in->n + 1;
            CX_BT_F(unhook)(g, j - 1);
            cx_free(in, sizeof(CX_BT_INNER));
        } else {
            in->k[in->n] = g->k[j];
            __builtin_memcpy(in->k + in->n + 1, right->k, right->n * sizeof(CX_BT_K));
            __builtin_memcpy(in->c + in->n + 1, right->c, (right->n + 1) * sizeof(void*));
            in->n += right->n + 1;
            CX_BT_F(unhook)(g, j);
            cx_free(right, sizeof(CX_BT_INNER));
        }
        h--;
    }
}

/* Takes place `i` out of the leaf `l` that `path` and `at` lead to, once its key and value are
   freed or moved out, and keeps the tree balanced. */
static void CX_BT_F(take)(CX_BT_T* t, CX_BT_INNER** path, const cx_u32* at, CX_BT_LEAF* l, cx_u32 i) {
    __builtin_memmove(l->k + i, l->k + i + 1, (l->n - i - 1) * sizeof(CX_BT_K));
    CX_BT_WITH_V(__builtin_memmove(l->v + i, l->v + i + 1, (l->n - i - 1) * sizeof(CX_BT_V));)
    l->n--;
    t->len--;
    if (l->n >= CX_BT_LMIN) return;
    if (!t->height) {
        if (!l->n) {
            cx_free(l, sizeof(CX_BT_LEAF));
            t->root = t->first = t->last = 0;
        }
        return;
    }
    CX_BT_INNER* g = path[t->height - 1];
    cx_u32 j = at[t->height - 1];
    CX_BT_LEAF* left = j ? (CX_BT_LEAF*)g->c[j - 1] : 0;
    CX_BT_LEAF* right = j < g->n ? (CX_BT_LEAF*)g->c[j + 1] : 0;
    if (left && left->n > CX_BT_LMIN) {
        __builtin_memmove(l->k + 1, l->k, l->n * sizeof(CX_BT_K));
        CX_BT_WITH_V(__builtin_memmove(l->v + 1, l->v, l->n * sizeof(CX_BT_V));)
        l->k[0] = left->k[left->n - 1];
        CX_BT_WITH_V(l->v[0] = left->v[left->n - 1];)
        l->n++;
        left->n--;
        CX_BT_DROP_SEP(&g->k[j - 1]);
        g->k[j - 1] = CX_BT_SEP(&l->k[0]);
        return;
    }
    if (right && right->n > CX_BT_LMIN) {
        l->k[l->n] = right->k[0];
        CX_BT_WITH_V(l->v[l->n] = right->v[0];)
        l->n++;
        __builtin_memmove(right->k, right->k + 1, (right->n - 1) * sizeof(CX_BT_K));
        CX_BT_WITH_V(__builtin_memmove(right->v, right->v + 1, (right->n - 1) * sizeof(CX_BT_V));)
        right->n--;
        CX_BT_DROP_SEP(&g->k[j]);
        g->k[j] = CX_BT_SEP(&right->k[0]);
        return;
    }
    if (left) {
        CX_BT_F(merge_leaves)(t, left, l);
        CX_BT_DROP_SEP(&g->k[j - 1]);
        CX_BT_F(unhook)(g, j - 1);
    } else {
        CX_BT_F(merge_leaves)(t, l, right);
        CX_BT_DROP_SEP(&g->k[j]);
        CX_BT_F(unhook)(g, j);
    }
    CX_BT_F(fix_inner)(t, path, at, t->height - 1);
}

/* Takes the key `x` out, freeing it, and moves its value to `*out`; whether it was there. */
static cx_bool CX_BT_F(del)(CX_BT_T* t, CX_BT_KL x CX_BT_WITH_V(, CX_BT_V* out)) {
    if (!t->root) return 0;
    CX_BT_INNER* path[CX_BT_DEPTH];
    cx_u32 at[CX_BT_DEPTH];
    void* p = t->root;
    for (cx_u32 h = 0; h < t->height; h++) {
        CX_BT_INNER* in = (CX_BT_INNER*)p;
        cx_u32 i = CX_BT_F(upper)(in->k, in->n, x);
        path[h] = in;
        at[h] = i;
        p = in->c[i];
    }
    CX_BT_LEAF* l = (CX_BT_LEAF*)p;
    cx_u32 i = CX_BT_F(lower)(l->k, l->n, x);
    if (i == l->n || CX_BT_LESS(x, CX_BT_LOOK(l->k[i]))) return 0;
    CX_BT_DROP_K(&l->k[i]);
    CX_BT_WITH_V(*out = l->v[i];)
    CX_BT_F(take)(t, path, at, l, i);
    return 1;
}

/* Moves the first key out, or the last, to `*k`, with its value; whether there was one. */
static cx_bool CX_BT_F(pop)(CX_BT_T* t, cx_bool last, CX_BT_K* k CX_BT_WITH_V(, CX_BT_V* v)) {
    if (!t->root) return 0;
    CX_BT_INNER* path[CX_BT_DEPTH];
    cx_u32 at[CX_BT_DEPTH];
    void* p = t->root;
    for (cx_u32 h = 0; h < t->height; h++) {
        CX_BT_INNER* in = (CX_BT_INNER*)p;
        cx_u32 i = last ? in->n : 0;
        path[h] = in;
        at[h] = i;
        p = in->c[i];
    }
    CX_BT_LEAF* l = (CX_BT_LEAF*)p;
    cx_u32 i = last ? l->n - 1 : 0;
    *k = l->k[i];
    CX_BT_WITH_V(*v = l->v[i];)
    CX_BT_F(take)(t, path, at, l, i);
    return 1;
}

static void CX_BT_F(free_node)(void* p, cx_u32 h) {
    if (!h) {
        CX_BT_LEAF* l = (CX_BT_LEAF*)p;
#if defined(CX_BT_COPY_K) || defined(CX_BT_COPY_V)
        for (cx_u32 i = 0; i < l->n; i++) {
            CX_BT_DROP_K(&l->k[i]);
            CX_BT_DROP_V(&l->v[i]);
        }
#endif
        cx_free(l, sizeof(CX_BT_LEAF));
        return;
    }
    CX_BT_INNER* in = (CX_BT_INNER*)p;
    for (cx_u32 i = 0; i <= in->n; i++) CX_BT_F(free_node)(in->c[i], h - 1);
#ifdef CX_BT_COPY_K
    for (cx_u32 i = 0; i < in->n; i++) CX_BT_DROP_SEP(&in->k[i]);
#endif
    cx_free(in, sizeof(CX_BT_INNER));
}

/* Frees every key and value, and every node. */
static void CX_BT_F(clear)(CX_BT_T* t) {
    if (t->root) CX_BT_F(free_node)(t->root, t->height);
    t->root = t->first = t->last = 0;
    t->len = 0;
    t->height = 0;
}

static void* CX_BT_F(copy_node)(const void* p, cx_u32 h, CX_BT_LEAF** last) {
    if (!h) {
        const CX_BT_LEAF* l = (const CX_BT_LEAF*)p;
        CX_BT_LEAF* r = CX_BT_F(new_leaf)();
        r->n = l->n;
#ifdef CX_BT_COPY_K
        for (cx_u32 i = 0; i < l->n; i++) r->k[i] = CX_BT_COPY_K(&l->k[i]);
#else
        __builtin_memcpy(r->k, l->k, l->n * sizeof(CX_BT_K));
#endif
#ifdef CX_BT_COPY_V
        for (cx_u32 i = 0; i < l->n; i++) r->v[i] = CX_BT_COPY_V(&l->v[i]);
#else
        CX_BT_WITH_V(__builtin_memcpy(r->v, l->v, l->n * sizeof(CX_BT_V));)
#endif
        r->prev = *last;
        if (*last) (*last)->next = r;
        *last = r;
        return r;
    }
    const CX_BT_INNER* in = (const CX_BT_INNER*)p;
    CX_BT_INNER* r = CX_BT_F(new_inner)();
    r->n = in->n;
    for (cx_u32 i = 0; i < in->n; i++) r->k[i] = CX_BT_SEP(&in->k[i]);
    for (cx_u32 i = 0; i <= in->n; i++) r->c[i] = CX_BT_F(copy_node)(in->c[i], h - 1, last);
    return r;
}

/* A copy of the tree, the same shape, that owns memory of its own. */
static CX_BT_T CX_BT_F(copy)(const CX_BT_T* t) {
    CX_BT_T r = {0};
    if (!t->root) return r;
    CX_BT_LEAF* last = 0;
    r.root = CX_BT_F(copy_node)(t->root, t->height, &last);
    r.last = last;
    void* p = r.root;
    for (cx_u32 h = t->height; h; h--) p = ((CX_BT_INNER*)p)->c[0];
    r.first = p;
    r.len = t->len;
    r.height = t->height;
    return r;
}

#undef CX_BT_CAT2
#undef CX_BT_CAT
#undef CX_BT_F
#undef CX_BT_LEAF
#undef CX_BT_INNER
#undef CX_BT_VSIZE
#undef CX_BT_ESIZE
#undef CX_BT_WITH_V
#undef CX_BT_SEP
#undef CX_BT_DROP_SEP
#undef CX_BT_LCAP
#undef CX_BT_ICAP
#undef CX_BT_LMIN
#undef CX_BT_IMIN
#undef CX_BT_DEPTH
#undef CX_BT_T
#undef CX_BT_NAME
#undef CX_BT_K
#undef CX_BT_V
#undef CX_BT_KL
#undef CX_BT_LOOK
#undef CX_BT_LESS
#undef CX_BT_SCALAR
#undef CX_BT_DROP_K
#undef CX_BT_DROP_V
#undef CX_BT_COPY_K
#undef CX_BT_COPY_V
