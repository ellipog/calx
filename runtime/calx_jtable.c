/* JavaHashMap and JavaHashSet (D105): Java's HashMap, in Java's order, for the same operations.
 *
 * What Java lets a program see of a HashMap is its order, and that's fixed by a few rules: a key
 * goes in bin `spread(hashCode) & (bins - 1)`, at the end of the bin; the table doubles when it
 * holds more than three quarters of its bins, and each bin splits into the keys that stay and the
 * keys that move up by the old size, each in the order they were; and a bin of eight or more keys
 * in a table of at least 64 bins becomes a red-black tree, whose root goes to the front of the
 * bin, with a new key after its parent in the tree. This file is those rules, written for Calx's
 * layout and checked against Java 25 by running both on the same operations: it's no port of
 * Java's code.
 *
 * The parts here don't depend on the key's type. The compiler writes the rest for each table:
 * hashing, comparing and finding keys, and the inline fast paths.
 */
#include "calx_rt.h"

#define HASH(i) CX_JT_HASH(m->nodes, size, i)
#define NEXT(i) CX_JT_NEXT(m->nodes, size, i)

enum {
    TREE_AT = 8,        /* a bin becomes a tree when a key is added after its 8th... */
    TREE_BINS = 64,     /* ...in a table of at least this many bins; a smaller one doubles */
    LIST_AT = 6,        /* a tree that splits into this many keys or fewer becomes a list */
};
#define MAX_BINS (1u << 30)
#define MAX_THR 0x7FFFFFFFu

cx_u32 cx_jt_size_for(cx_usize n) {
    if (n <= 1) return 1;
    if (n >= MAX_BINS) return MAX_BINS;
    return 1u << (32 - __builtin_clz((cx_u32)n - 1));
}

/* The threshold of a table of `bins` bins: three quarters of them, rounded down. */
static cx_u32 threshold(cx_u32 bins) { return bins >= MAX_BINS ? MAX_THR : (cx_u32)(((cx_u64)bins * 3) >> 2); }

static void grow_nodes(cx_jtable* m, cx_usize size) {
    cx_u32 room = m->room ? m->room * 2 : 8;
    m->nodes = cx_realloc(m->nodes, (cx_usize)m->room * size, (cx_usize)room * size);
    if (m->tree) m->tree = cx_realloc(m->tree, (cx_usize)m->room * sizeof(cx_jtnode), (cx_usize)room * sizeof(cx_jtnode));
    m->room = room;
}

cx_i32 cx_jt_new_node(cx_jtable* m, cx_usize size) {
    if (m->spare) {
        cx_i32 i = (cx_i32)m->spare - 1;
        m->spare = (cx_u32)(NEXT(i) + 1);
        return i;
    }
    if (m->used == m->room) grow_nodes(m, size);
    return (cx_i32)m->used++;
}

/* --- red-black trees ------------------------------------------------------------------------
 *
 * The balancing of Cormen, Leiserson, Rivest and Stein's Introduction to Algorithms, on the tree
 * links beside the nodes, with -1 for none. The shape that gives decides where a root and a new
 * key go in a bin's order, so it's the same balancing Java's trees have, case for case. */

#define L(i) t[i].left
#define R(i) t[i].right
#define P(i) t[i].parent
static cx_bool red(const cx_jtnode* t, cx_i32 i) { return i >= 0 && t[i].red; }

static void rotate_left(cx_jtnode* t, cx_i32* root, cx_i32 x) {
    cx_i32 y = R(x);
    R(x) = L(y);
    if (L(y) >= 0) P(L(y)) = x;
    P(y) = P(x);
    if (P(x) < 0) *root = y;
    else if (x == L(P(x))) L(P(x)) = y;
    else R(P(x)) = y;
    L(y) = x;
    P(x) = y;
}

static void rotate_right(cx_jtnode* t, cx_i32* root, cx_i32 x) {
    cx_i32 y = L(x);
    L(x) = R(y);
    if (R(y) >= 0) P(R(y)) = x;
    P(y) = P(x);
    if (P(x) < 0) *root = y;
    else if (x == R(P(x))) R(P(x)) = y;
    else L(P(x)) = y;
    R(y) = x;
    P(x) = y;
}

/* `x`, just linked in as a leaf, made red, and the colors fixed going up. */
static cx_i32 insert_fixup(cx_jtnode* t, cx_i32 root, cx_i32 x) {
    t[x].red = 1;
    while (red(t, P(x))) {
        cx_i32 xp = P(x), xpp = P(xp);
        if (xp == L(xpp)) {
            cx_i32 uncle = R(xpp);
            if (red(t, uncle)) {
                t[xp].red = 0;
                t[uncle].red = 0;
                t[xpp].red = 1;
                x = xpp;
                continue;
            }
            if (x == R(xp)) {
                x = xp;
                rotate_left(t, &root, x);
                xp = P(x);
                xpp = P(xp);
            }
            t[xp].red = 0;
            t[xpp].red = 1;
            rotate_right(t, &root, xpp);
        } else {
            cx_i32 uncle = L(xpp);
            if (red(t, uncle)) {
                t[xp].red = 0;
                t[uncle].red = 0;
                t[xpp].red = 1;
                x = xpp;
                continue;
            }
            if (x == L(xp)) {
                x = xp;
                rotate_right(t, &root, x);
                xp = P(x);
                xpp = P(xp);
            }
            t[xp].red = 0;
            t[xpp].red = 1;
            rotate_left(t, &root, xpp);
        }
    }
    t[root].red = 0;
    return root;
}

/* After a black node left the tree, `x`, in its place, carries an extra black up until it can
 * be dropped. `x` can be the node leaving, still linked, standing for the empty place. */
static cx_i32 delete_fixup(cx_jtnode* t, cx_i32 root, cx_i32 x) {
    while (x != root && !t[x].red) {
        cx_i32 xp = P(x);
        if (x == L(xp)) {
            cx_i32 w = R(xp);
            if (t[w].red) {
                t[w].red = 0;
                t[xp].red = 1;
                rotate_left(t, &root, xp);
                w = R(xp);
            }
            if (!red(t, L(w)) && !red(t, R(w))) {
                t[w].red = 1;
                x = xp;
                continue;
            }
            if (!red(t, R(w))) {
                t[L(w)].red = 0;
                t[w].red = 1;
                rotate_right(t, &root, w);
                w = R(xp);
            }
            t[w].red = t[xp].red;
            t[xp].red = 0;
            t[R(w)].red = 0;
            rotate_left(t, &root, xp);
        } else {
            cx_i32 w = L(xp);
            if (t[w].red) {
                t[w].red = 0;
                t[xp].red = 1;
                rotate_right(t, &root, xp);
                w = L(xp);
            }
            if (!red(t, L(w)) && !red(t, R(w))) {
                t[w].red = 1;
                x = xp;
                continue;
            }
            if (!red(t, L(w))) {
                t[R(w)].red = 0;
                t[w].red = 1;
                rotate_left(t, &root, w);
                w = L(xp);
            }
            t[w].red = t[xp].red;
            t[xp].red = 0;
            t[L(w)].red = 0;
            rotate_right(t, &root, xp);
        }
        x = root;
    }
    t[x].red = 0;
    return root;
}

/* --- tree bins ------------------------------------------------------------------------------ */

/* Moves the tree's root to the front of bin `b`'s order, and marks the bin a tree. */
static void root_to_front(cx_jtable* m, cx_usize size, cx_u32 b, cx_i32 root) {
    cx_jtnode* t = m->tree;
    cx_i32 first = cx_jt_first(m->heads[b]);
    if (root != first) {
        cx_i32 before = t[root].prev, after = NEXT(root);
        if (after >= 0) t[after].prev = before;
        if (before >= 0) NEXT(before) = after;
        t[first].prev = root;
        NEXT(root) = first;
        t[root].prev = -1;
    }
    m->heads[b] = -(root + 1);
}

/* Where `x` goes under the tree's node `p`: left for a smaller hash, then for a key that orders
 * first, and left too for one the order can't tell apart, where Java's order would depend on
 * addresses. */
static int direction(cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32 x, cx_i32 p) {
    cx_i32 h = HASH(x), ph = HASH(p);
    if (ph > h) return -1;
    if (ph < h) return 1;
    int d = order ? order(m->nodes, x, p) : 0;
    return d > 0 ? 1 : -1;
}

/* Links the node `x` in under the tree whose root is `root`, without balancing, and gives its
 * parent. */
static cx_i32 tree_link(cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32 root, cx_i32 x) {
    cx_jtnode* t = m->tree;
    cx_i32 p = root, parent;
    int dir;
    do {
        parent = p;
        dir = direction(m, size, order, x, p);
        p = dir < 0 ? t[p].left : t[p].right;
    } while (p >= 0);
    t[x].parent = parent;
    t[x].left = t[x].right = -1;
    if (dir < 0) t[parent].left = x;
    else t[parent].right = x;
    return parent;
}

/* Makes the list in bin `b` a tree, keeping its order but for the root, which goes first. */
static void treeify(cx_jtable* m, cx_usize size, cx_jt_order order, cx_u32 b) {
    if (!m->tree) m->tree = cx_alloc((cx_usize)m->room * sizeof(cx_jtnode));
    cx_jtnode* t = m->tree;
    cx_i32 root = -1, prev = -1;
    for (cx_i32 x = cx_jt_first(m->heads[b]); x >= 0; prev = x, x = NEXT(x)) {
        t[x].prev = prev;
        if (root < 0) {
            t[x].parent = t[x].left = t[x].right = -1;
            t[x].red = 0;
            root = x;
            continue;
        }
        tree_link(m, size, order, root, x);
        root = insert_fixup(t, root, x);
    }
    root_to_front(m, size, b, root);
}

void cx_jt_tree_put(cx_jtable* m, cx_usize size, cx_jt_order order, cx_u32 b, cx_i32 x) {
    cx_jtnode* t = m->tree;
    cx_i32 root = cx_jt_first(m->heads[b]);
    cx_i32 parent = tree_link(m, size, order, root, x);
    /* In the bin's order, a new key comes right after its parent in the tree. */
    cx_i32 after = NEXT(parent);
    NEXT(x) = after;
    NEXT(parent) = x;
    t[x].prev = parent;
    if (after >= 0) t[after].prev = x;
    root_to_front(m, size, b, insert_fixup(t, root, x));
}

void cx_jt_tree_remove(cx_jtable* m, cx_usize size, cx_u32 b, cx_i32 z) {
    cx_jtnode* t = m->tree;
    cx_i32 root = cx_jt_first(m->heads[b]);
    /* Out of the bin's order first. */
    cx_i32 before = t[z].prev, after = NEXT(z), first = root;
    if (before < 0) {
        first = after;
        m->heads[b] = after >= 0 ? -(after + 1) : 0;
    } else {
        NEXT(before) = after;
    }
    if (after >= 0) t[after].prev = before;
    if (first < 0) return;
    /* A tree that's this small, counting `z`, becomes a list of what's left, in its order. */
    if (t[root].right < 0 || t[root].left < 0 || t[t[root].left].left < 0) {
        m->heads[b] = first + 1;
        return;
    }
    /* Then out of the tree. A node with two children first trades places and colors with the
     * next node in the tree, the leftmost of its right side, so it has at most one child. */
    if (t[z].left >= 0 && t[z].right >= 0) {
        cx_i32 s = t[z].right;
        while (t[s].left >= 0) s = t[s].left;
        cx_bool c = t[s].red;
        t[s].red = t[z].red;
        t[z].red = c;
        cx_i32 zp = t[z].parent, zl = t[z].left, zr = t[z].right, sr = t[s].right;
        if (s == zr) {
            t[s].right = z;
            t[z].parent = s;
        } else {
            cx_i32 sp = t[s].parent;
            if (s == t[sp].left) t[sp].left = z;
            else t[sp].right = z;
            t[z].parent = sp;
            t[s].right = zr;
            t[zr].parent = s;
        }
        t[z].left = -1;
        t[z].right = sr;
        if (sr >= 0) t[sr].parent = z;
        t[s].left = zl;
        t[zl].parent = s;
        t[s].parent = zp;
        if (zp < 0) root = s;
        else if (z == t[zp].left) t[zp].left = s;
        else t[zp].right = s;
    }
    cx_i32 child = t[z].left >= 0 ? t[z].left : t[z].right;
    if (child >= 0) {
        /* The child takes `z`'s place. */
        cx_i32 zp = t[z].parent;
        t[child].parent = zp;
        if (zp < 0) root = child;
        else if (z == t[zp].left) t[zp].left = child;
        else t[zp].right = child;
        if (!t[z].red) root = delete_fixup(t, root, child);
    } else {
        /* A leaf: it stands for its empty place while the colors are fixed, then goes. */
        if (!t[z].red) root = delete_fixup(t, root, z);
        cx_i32 zp = t[z].parent;
        if (zp >= 0) {
            if (z == t[zp].left) t[zp].left = -1;
            else t[zp].right = -1;
        }
    }
    root_to_front(m, size, b, root);
}

void cx_jt_treeify_bin(cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32 h) {
    if (m->cap < TREE_BINS) {
        cx_jt_resize(m, size, order);
        return;
    }
    treeify(m, size, order, (cx_u32)h & (m->cap - 1));
}

/* --- growing ------------------------------------------------------------------------------- */

/* Bin `j`'s keys, in order, split between bin `j` and bin `j + old` of the new heads `to`. */
static void split(cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32* to, cx_u32 j, cx_u32 old) {
    cx_i32 head = m->heads[j];
    cx_i32 lo = -1, lo_end = -1, hi = -1, hi_end = -1;
    cx_u32 lo_n = 0, hi_n = 0;
    cx_jtnode* t = m->tree;
    for (cx_i32 x = cx_jt_first(head), next; x >= 0; x = next) {
        next = NEXT(x);
        NEXT(x) = -1;
        if ((cx_u32)HASH(x) & old) {
            if (head < 0) t[x].prev = hi_end;
            if (hi_end < 0) hi = x;
            else NEXT(hi_end) = x;
            hi_end = x;
            hi_n++;
        } else {
            if (head < 0) t[x].prev = lo_end;
            if (lo_end < 0) lo = x;
            else NEXT(lo_end) = x;
            lo_end = x;
            lo_n++;
        }
    }
    if (lo >= 0) to[j] = lo + 1;
    if (hi >= 0) to[j + old] = hi + 1;
    if (head > 0) return;
    /* A tree's halves: a small one becomes a list, and a big one is a tree again, the same tree
     * if the other half is empty. */
    cx_i32* from = m->heads;
    m->heads = to;
    if (lo >= 0 && lo_n > LIST_AT) {
        if (hi < 0) to[j] = -(lo + 1);
        else treeify(m, size, order, j);
    }
    if (hi >= 0 && hi_n > LIST_AT) {
        if (lo < 0) to[j + old] = -(hi + 1);
        else treeify(m, size, order, j + old);
    }
    m->heads = from;
}

void cx_jt_resize(cx_jtable* m, cx_usize size, cx_jt_order order) {
    cx_u32 old = m->cap, cap;
    if (old >= MAX_BINS) {
        m->thr = MAX_THR;
        return;
    }
    /* Before the first table, `thr` is the size Java's `new HashMap<>(n)` asked for. */
    cap = old ? old * 2 : m->thr ? m->thr : 16;
    cx_i32* heads = cx_alloc((cx_usize)cap * sizeof(cx_i32));
    __builtin_memset(heads, 0, (cx_usize)cap * sizeof(cx_i32));
    for (cx_u32 j = 0; j < old; j++) {
        cx_i32 head = m->heads[j];
        if (head == 0) continue;
        cx_i32 x = cx_jt_first(head);
        if (head > 0 && NEXT(x) < 0) heads[(cx_u32)HASH(x) & (cap - 1)] = x + 1;
        else split(m, size, order, heads, j, old);
    }
    cx_free(m->heads, (cx_usize)old * sizeof(cx_i32));
    m->heads = heads;
    m->cap = cap;
    m->thr = threshold(cap);
}

/* What Java's `putAll` does before it adds `n` keys: a table that doesn't exist yet gets room for
 * them, and one that does doubles until its threshold reaches `n`. */
void cx_jt_reserve(cx_jtable* m, cx_usize size, cx_jt_order order, cx_usize n) {
    if (n == 0) return;
    if (!m->cap) {
        cx_usize want = n > (cx_usize)MAX_BINS ? (cx_usize)MAX_BINS : (n * 4 + 2) / 3;  /* n / 0.75, rounded up */
        if (want > MAX_BINS) want = MAX_BINS;
        if (want > m->thr) m->thr = cx_jt_size_for(want);
        return;
    }
    while (n > m->thr && m->cap < MAX_BINS) cx_jt_resize(m, size, order);
}

/* --- the whole table ------------------------------------------------------------------------ */

void cx_jt_free(cx_jtable* m, cx_usize size) {
    cx_free(m->heads, (cx_usize)m->cap * sizeof(cx_i32));
    cx_free(m->nodes, (cx_usize)m->room * size);
    cx_free(m->tree, (cx_usize)m->room * sizeof(cx_jtnode));
}

/* Every array copied as it is, so the copy has the same bins, trees and order; the compiler
 * copies what the keys and values own. */
cx_jtable cx_jt_copy(const cx_jtable* m, cx_usize size) {
    cx_jtable r = *m;
    r.heads = cx_alloc((cx_usize)m->cap * sizeof(cx_i32));
    __builtin_memcpy(r.heads, m->heads, (cx_usize)m->cap * sizeof(cx_i32));
    r.room = m->used;
    r.nodes = 0;
    r.tree = 0;
    if (m->used) {
        r.nodes = cx_alloc((cx_usize)m->used * size);
        __builtin_memcpy(r.nodes, m->nodes, (cx_usize)m->used * size);
        if (m->tree) {
            r.tree = cx_alloc((cx_usize)m->used * sizeof(cx_jtnode));
            __builtin_memcpy(r.tree, m->tree, (cx_usize)m->used * sizeof(cx_jtnode));
        }
    }
    return r;
}

/* Java's `clear` keeps the table's size. */
void cx_jt_clear(cx_jtable* m) {
    if (m->heads) __builtin_memset(m->heads, 0, (cx_usize)m->cap * sizeof(cx_i32));
    m->len = 0;
    m->used = 0;
    m->spare = 0;
}

int cx_jt_str_order(cx_str a, cx_str b) {
    /* Java orders strings by UTF-16 unit, UTF-8 by code point. They differ only where a rune past
     * U+FFFF, whose first unit is a surrogate, meets one from U+E000 to U+FFFF. */
    cx_usize n = a.len < b.len ? a.len : b.len, d = 0;
    while (d < n && a.ptr[d] == b.ptr[d]) d++;
    if (d == n) return a.len < b.len ? -1 : a.len > b.len;
    cx_usize s = d;
    while (s > 0 && (a.ptr[s] & 0xC0) == 0x80) s--;
    cx_u8 x = a.ptr[s], y = b.ptr[s];
    if (x >= 0xEE && x <= 0xEF && y >= 0xF0) return 1;
    if (y >= 0xEE && y <= 0xEF && x >= 0xF0) return -1;
    return a.ptr[d] < b.ptr[d] ? -1 : 1;
}
