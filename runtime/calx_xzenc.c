/* LZMA and LZMA2 compression, behind std.xz's `compress` (CLX-176). std.xz writes the containers;
 * this writes what their blocks hold. Linked only into programs that compress.
 *
 * Written from the .xz file format spec 1.2.1 and the LZMA SDK's account of how LZMA is encoded,
 * which Igor Pavlov put in the public domain: a range coder, match finders that keep the text's
 * positions in hash chains or binary trees, and the SDK's two parsers. The fast one takes the
 * longest match unless the next byte starts a better one; the optimal one prices every way through
 * the next few thousand bytes, from the coder's own probabilities, and takes the cheapest. No
 * liblzma code is in it.
 *
 * LZMA2 cuts a block into chunks of at most 2 MiB of text and 64 KiB of code, each with its own
 * range coder, and a chunk may reset the coder's state without forgetting the text before it. So a
 * block is cut into slices of 1 MiB that compress at once, on `parallel for`'s threads: each slice
 * starts with a state reset, but matches still reach back across the cut, into text a decoder has
 * by then. For presets 0 to 3, each slice's hash chains first take in the dictionary before it.
 * For presets 4 to 9, whose binary trees take in text slowly, the threads share the trees out by
 * hash, find all the block's matches, and then parse the slices from them. What comes out is one
 * LZMA2 stream that any decoder reads in order, a few bytes bigger for each reset, and the same
 * however many threads made it. A .lzma file is one LZMA stream with no resets, so one thread parses
 * it, from the matches the threads find a window ahead. */
#include "calx_rt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define BITS 11
#define ONE (1u << BITS)
#define MOVE 5
#define TOP (1u << 24)

#define STATES 12
#define POS_STATES_MAX 16
#define DIST_STATES 4
#define END_POS_MODEL 14
#define FULL_DISTANCES 128
#define ALIGN_BITS 4
#define MATCH_MIN 2
#define MATCH_MAX 273
#define LEN_SYMBOLS 272
#define REPS 4

/* How far the optimal parser looks ahead, and a price no path costs. */
#define OPTS 4096
#define INF (1u << 30)
#define LITERAL 0xFFFFFFFFu

/* An LZMA2 chunk's most text and most code. */
#define CHUNK_TEXT ((cx_usize)1 << 21)
#define CHUNK_CODE ((cx_usize)1 << 16)
/* The most code one symbol can take, with the range coder's bytes still to come; and the most a
 * byte of text can take as part of any symbol, which bounds how far a parse may run in a chunk. */
#define SYMBOL_CODE 64
#define CODE_PER_BYTE 10

#define LIT_NEXT(s) ((s) < 4 ? 0 : (s) < 10 ? (s) - 3 : (s) - 6)
#define MATCH_NEXT(s) ((s) < 7 ? 7 : 10)
#define REP_NEXT(s) ((s) < 7 ? 8 : 11)
#define SHORTREP_NEXT(s) ((s) < 7 ? 9 : 11)

typedef cx_u16 prob;

/* --- prices ---------------------------------------------------------------------------------- */

/* A bit's price is how much code it takes, -log2 of its probability, in sixteenths of a bit, from
 * a table of 128 probabilities, as the SDK keeps it. */
static cx_u32 price_table[ONE >> 4];
/* Mixes a byte into the match finders' hashes. */
static cx_u32 hash_table[256];
static int tables_ready;

/* log2(v) in 16.16 fixed point, for v from 1: the whole part from the top bit, and each bit of the
 * fraction from squaring what's left. */
static cx_u32 log2_q16(cx_u32 v) {
    int top = 31 - __builtin_clz(v);
    cx_u64 y = ((cx_u64)v << 31) >> top; /* v / 2^top, in [1, 2), as 1.31 */
    cx_u32 r = (cx_u32)top << 16;
    for (int i = 15; i >= 0; i--) {
        y = (y * y) >> 31;
        if (y >= ((cx_u64)2 << 31)) {
            y >>= 1;
            r |= 1u << i;
        }
    }
    return r;
}

static void tables_init(void) {
    for (cx_u32 i = 0; i < (ONE >> 4); i++) {
        cx_u32 p = (i << 4) + 8; /* the middle of the sixteen the entry stands for */
        cx_u32 bits = (11u << 16) - log2_q16(p);
        price_table[i] = (bits + (1u << 11)) >> 12;
    }
    for (cx_u32 i = 0; i < 256; i++) {
        cx_u32 h = i * 0x9E3779B1u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        hash_table[i] = h;
    }
    __atomic_store_n(&tables_ready, 1, __ATOMIC_RELEASE);
}

#define P0(p) price_table[(p) >> 4]
#define P1(p) price_table[(ONE - (p)) >> 4]
#define PB(p, b) ((b) ? P1(p) : P0(p))

/* --- the range coder ------------------------------------------------------------------------- */

/* `low` holds the code's next bytes; a carry out of it adds one to the byte in `cache` and turns
 * the `pending` - 1 0xFF bytes after it to zeros, so those wait until it's known there's none. */
typedef struct {
    cx_u64 low;
    cx_u32 range;
    cx_u8 cache;
    cx_usize pending;
    cx_u8* out;
    cx_usize n;
} rc_t;

static void rc_start(rc_t* r, cx_u8* out) {
    r->low = 0;
    r->range = 0xFFFFFFFFu;
    r->cache = 0;
    r->pending = 1;
    r->out = out;
    r->n = 0;
}

static inline void rc_shift(rc_t* r) {
    if ((cx_u32)r->low < 0xFF000000u || (r->low >> 32) != 0) {
        cx_u8 carry = (cx_u8)(r->low >> 32), b = r->cache;
        do {
            r->out[r->n++] = (cx_u8)(b + carry);
            b = 0xFF;
        } while (--r->pending);
        r->cache = (cx_u8)(r->low >> 24);
    }
    r->pending++;
    r->low = (r->low & 0x00FFFFFFu) << 8;
}

static void rc_flush(rc_t* r) {
    for (int i = 0; i < 5; i++) rc_shift(r);
}

/* How many bytes the code takes if it's flushed now, at most. */
static inline cx_usize rc_size(const rc_t* r) { return r->n + r->pending + 5; }

static inline void rc_bit(rc_t* r, prob* p, cx_u32 bit) {
    cx_u32 bound = (r->range >> BITS) * *p;
    if (!bit) {
        r->range = bound;
        *p += (prob)((ONE - *p) >> MOVE);
    } else {
        r->low += bound;
        r->range -= bound;
        *p -= (prob)(*p >> MOVE);
    }
    if (r->range < TOP) {
        r->range <<= 8;
        rc_shift(r);
    }
}

/* The low `n` bits of `v`, most significant first, at even odds. */
static inline void rc_direct(rc_t* r, cx_u32 v, cx_u32 n) {
    do {
        r->range >>= 1;
        n--;
        r->low += r->range & (0u - ((v >> n) & 1));
        if (r->range < TOP) {
            r->range <<= 8;
            rc_shift(r);
        }
    } while (n);
}

/* `sym`'s `bits` bits down the tree at `probs`, most significant first. */
static inline void rc_tree(rc_t* r, prob* probs, cx_u32 bits, cx_u32 sym) {
    cx_u32 m = 1;
    for (cx_u32 i = bits; i-- > 0;) {
        cx_u32 b = (sym >> i) & 1;
        rc_bit(r, &probs[m], b);
        m = (m << 1) | b;
    }
}

/* The same, least significant first. */
static inline void rc_rtree(rc_t* r, prob* probs, cx_u32 bits, cx_u32 sym) {
    cx_u32 m = 1;
    for (cx_u32 i = 0; i < bits; i++) {
        cx_u32 b = sym & 1;
        sym >>= 1;
        rc_bit(r, &probs[m], b);
        m = (m << 1) | b;
    }
}

static cx_u32 rtree_price(const prob* probs, cx_u32 bits, cx_u32 sym) {
    cx_u32 price = 0, m = 1;
    for (cx_u32 i = 0; i < bits; i++) {
        cx_u32 b = sym & 1;
        sym >>= 1;
        price += PB(probs[m], b);
        m = (m << 1) | b;
    }
    return price;
}

/* The price of each of a tree's 2^bits symbols, plus `add`, into out: each node's price is its
 * parent's and the bit to it, so the whole tree takes a pass. */
static void tree_prices(const prob* probs, cx_u32 bits, cx_u32 add, cx_u32* out) {
    cx_u32 node[512];
    cx_u32 n = 1u << bits;
    node[1] = add;
    for (cx_u32 k = 1; k < n; k++) {
        node[2 * k] = node[k] + P0(probs[k]);
        node[2 * k + 1] = node[k] + P1(probs[k]);
    }
    memcpy(out, node + n, n * sizeof(cx_u32));
}

/* --- the match finders ------------------------------------------------------------------------ */

typedef struct {
    cx_u32 len, dist; /* dist is the distance less one */
} match_t;

/* The text's positions by hashes of their first two, three and four bytes, the latest of each.
 * Before them, a hash chain links each position to the one before with the same four-byte hash;
 * a binary tree keeps those positions sorted by the text that follows them, so a search goes
 * down one path and each step has as long a match as the one before.
 *
 * Positions are 32 bits, numbered from `size`, so 0 is always further back than a match reaches:
 * an empty slot needs no check of its own. */
typedef struct {
    const cx_u8* base;  /* base + pos is the byte at pos */
    cx_u64 base_at;     /* where base + 0 is, in the text */
    cx_u64 text_end;    /* where the text ends, from the same place */
    cx_u32 pos;         /* the next position */
    cx_u32 size;        /* the cyclic buffer's size, a power of two */
    cx_u32 reach;       /* how far back matches reach, less than size */
    cx_u32 nice, depth;
    cx_u32 hash_bits;
    int bt;
    cx_u32* head2; /* 1024 */
    cx_u32* head3; /* 65536 */
    cx_u32* head4;
    cx_u32* son;
} mf_t;

#define HEAD2 1024u
#define HEAD3 65536u
/* How far back the two- and three-byte matches reach, which pay only when they're near. */
#define SHORT_REACH (1u << 18)

static inline cx_u32 load32(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}

static inline cx_u64 load64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return v;
}

static inline cx_u32 load16(const cx_u8* p) {
    cx_u16 v;
    memcpy(&v, p, 2);
    return v;
}

/* How far a and b agree, counting from `len`, up to `limit`. */
static inline cx_u32 common(const cx_u8* a, const cx_u8* b, cx_u32 len, cx_u32 limit) {
    while (len + 8 <= limit) {
        cx_u64 x = load64(a + len) ^ load64(b + len);
        if (x) return len + ((cx_u32)__builtin_ctzll(x) >> 3);
        len += 8;
    }
    while (len < limit && a[len] == b[len]) len++;
    return len;
}

/* The bytes left from the next position, at most `cap`. */
static inline cx_u32 mf_avail(const mf_t* f, cx_u32 cap) {
    cx_u64 left = f->text_end - (f->base_at + f->pos);
    return left < cap ? (cx_u32)left : cap;
}

/* Renumbers the positions down when they near 2^32, keeping the last `size`. */
static void mf_normalize(mf_t* f) {
    cx_u32 sub = f->pos - f->size;
    cx_u32* tables[3] = {f->head2, f->head3, f->head4};
    cx_usize sizes[3] = {HEAD2, HEAD3, (cx_usize)1 << f->hash_bits};
    for (int t = 0; t < 3; t++) {
        cx_u32* h = tables[t];
        for (cx_usize i = 0; i < sizes[t]; i++) h[i] = h[i] > sub ? h[i] - sub : 0;
    }
    cx_usize sons = (cx_usize)f->size << f->bt;
    for (cx_usize i = 0; i < sons; i++) f->son[i] = f->son[i] > sub ? f->son[i] - sub : 0;
    f->pos -= sub;
    f->base += sub;
    f->base_at += sub;
}

static inline void mf_next(mf_t* f) {
    if (__builtin_expect(++f->pos >= 0xF0000000u, 0)) mf_normalize(f);
}

/* The four-byte hash, and the two- and three-byte ones, which mix the first byte the same way so
 * that two positions in the same slot with the same first byte have the same two or three. */
#define HASHES(cur)                                                                \
    cx_u32 v_ = load32(cur);                                                       \
    cx_u32 t_ = hash_table[(cur)[0]] ^ (cur)[1];                                   \
    cx_u32 h2 = t_ & (HEAD2 - 1);                                                  \
    cx_u32 h3 = (t_ ^ ((cx_u32)(cur)[2] << 8)) & (HEAD3 - 1);                      \
    cx_u32 h4 = (v_ * 0x9E3779B1u) >> (32 - f->hash_bits);

/* Both of a node's children, and the text at each, start coming in while the node's own text is
 * compared, so the step down finds them ready whichever way it goes. */
#define BT_AHEAD(pair, len)                                       \
    {                                                             \
        cx_u32 a_ = (pair)[0], b_ = (pair)[1];                    \
        __builtin_prefetch(son + 2 * (a_ & mask));                \
        __builtin_prefetch(son + 2 * (b_ & mask));                \
        __builtin_prefetch(cur - (f->pos - a_) + (len));          \
        __builtin_prefetch(cur - (f->pos - b_) + (len));          \
    }

/* Puts the position into the tree whose root is `cand`, without noting matches: the search for its
 * place, which stops at `limit` bytes in common, or at `depth` steps. */
static void bt_skip(mf_t* f, cx_u32 cand, cx_u32 limit) {
    const cx_u8* cur = f->base + f->pos;
    const cx_u32 mask = f->size - 1, reach = f->reach;
    cx_u32* son = f->son;
    cx_u32* ptr0 = son + 2 * (f->pos & mask) + 1;
    cx_u32* ptr1 = son + 2 * (f->pos & mask);
    cx_u32 len0 = 0, len1 = 0, depth = f->depth;
    for (;;) {
        cx_u32 delta = f->pos - cand;
        if (depth-- == 0 || delta > reach) {
            *ptr0 = *ptr1 = 0;
            return;
        }
        cx_u32* pair = son + 2 * (cand & mask);
        const cx_u8* pb = cur - delta;
        cx_u32 len = len0 < len1 ? len0 : len1;
        BT_AHEAD(pair, len)
        if (pb[len] == cur[len]) {
            len = common(pb, cur, len + 1, limit);
            if (len == limit) {
                *ptr1 = pair[0];
                *ptr0 = pair[1];
                return;
            }
        }
        if (pb[len] < cur[len]) {
            *ptr1 = cand;
            ptr1 = pair + 1;
            cand = *ptr1;
            len1 = len;
        } else {
            *ptr0 = cand;
            ptr0 = pair;
            cand = *ptr0;
            len0 = len;
        }
    }
}

/* The same, noting each match longer than `best` in m, and giving how many. */
static cx_u32 bt_find(mf_t* f, cx_u32 cand, cx_u32 limit, cx_u32 best, match_t* m) {
    const cx_u8* cur = f->base + f->pos;
    const cx_u32 mask = f->size - 1, reach = f->reach;
    cx_u32* son = f->son;
    cx_u32* ptr0 = son + 2 * (f->pos & mask) + 1;
    cx_u32* ptr1 = son + 2 * (f->pos & mask);
    cx_u32 len0 = 0, len1 = 0, depth = f->depth, count = 0;
    for (;;) {
        cx_u32 delta = f->pos - cand;
        if (depth-- == 0 || delta > reach) {
            *ptr0 = *ptr1 = 0;
            return count;
        }
        cx_u32* pair = son + 2 * (cand & mask);
        const cx_u8* pb = cur - delta;
        cx_u32 len = len0 < len1 ? len0 : len1;
        BT_AHEAD(pair, len)
        if (pb[len] == cur[len]) {
            len = common(pb, cur, len + 1, limit);
            if (len > best) {
                best = len;
                m[count].len = len;
                m[count].dist = delta - 1;
                count++;
                if (len == limit) {
                    *ptr1 = pair[0];
                    *ptr0 = pair[1];
                    return count;
                }
            }
        }
        if (pb[len] < cur[len]) {
            *ptr1 = cand;
            ptr1 = pair + 1;
            cand = *ptr1;
            len1 = len;
        } else {
            *ptr0 = cand;
            ptr0 = pair;
            cand = *ptr0;
            len0 = len;
        }
    }
}

/* The hash chain's search from `cand`, noting each match longer than `best`. */
static cx_u32 hc_find(mf_t* f, cx_u32 cand, cx_u32 limit, cx_u32 best, match_t* m) {
    const cx_u8* cur = f->base + f->pos;
    const cx_u32 mask = f->size - 1, reach = f->reach;
    cx_u32 depth = f->depth, count = 0;
    for (;;) {
        cx_u32 delta = f->pos - cand;
        if (depth-- == 0 || delta > reach) return count;
        const cx_u8* pb = cur - delta;
        cand = f->son[cand & mask];
        if (pb[best] == cur[best] && pb[0] == cur[0]) {
            cx_u32 len = common(pb, cur, 1, limit);
            if (len > best) {
                best = len;
                m[count].len = len;
                m[count].dist = delta - 1;
                count++;
                if (len == limit) return count;
            }
        }
    }
}

/* The four-byte head a search starts from starts coming in two positions before it, which measured
 * best: one and four positions were slower, and so was fetching the tree's first node too. */
static inline void mf_ahead(const mf_t* f, const cx_u8* cur, cx_u32 avail) {
    if (avail >= 6) __builtin_prefetch(f->head4 + ((load32(cur + 2) * 0x9E3779B1u) >> (32 - f->hash_bits)));
}

/* The matches at the next position, shortest first, into m, and moves on: how many there are. The
 * longest is followed past `nice` to the longest a match can be. */
static cx_u32 mf_find(mf_t* f, match_t* m) {
    cx_u32 avail = mf_avail(f, MATCH_MAX);
    cx_u32 limit = avail < f->nice ? avail : f->nice;
    if (limit < 4) {
        mf_next(f);
        return 0;
    }
    const cx_u8* cur = f->base + f->pos;
    HASHES(cur)
    mf_ahead(f, cur, avail);
    const cx_u32 short_reach = f->reach < SHORT_REACH ? f->reach : SHORT_REACH;
    cx_u32 d2 = f->pos - f->head2[h2], d3 = f->pos - f->head3[h3], cand = f->head4[h4];
    f->head2[h2] = f->head3[h3] = f->head4[h4] = f->pos;
    if (!f->bt) f->son[f->pos & (f->size - 1)] = cand;
    cx_u32 best = 1, count = 0;
    if (d2 <= short_reach && cur[-(ptrdiff_t)d2] == cur[0]) {
        best = 2;
        m[0].len = 2;
        m[0].dist = d2 - 1;
        count = 1;
    }
    if (d2 != d3 && d3 <= short_reach && cur[-(ptrdiff_t)d3] == cur[0]) {
        best = 3;
        m[count].dist = d3 - 1;
        count++;
        d2 = d3;
    }
    if (count) {
        best = common(cur - d2, cur, best, limit);
        m[count - 1].len = best;
        if (best == limit) {
            if (f->bt) bt_skip(f, cand, limit);
            goto done;
        }
    }
    if (best < 3) best = 3;
    count += f->bt ? bt_find(f, cand, limit, best, m + count) : hc_find(f, cand, limit, best, m + count);
done:
    if (count && m[count - 1].len == f->nice && avail > f->nice) {
        m[count - 1].len = common(cur - m[count - 1].dist - 1, cur, f->nice, avail);
    }
    mf_next(f);
    return count;
}

/* Takes the next `n` positions in without searching them. */
static void mf_skip(mf_t* f, cx_u32 n) {
    while (n--) {
        cx_u32 limit = mf_avail(f, f->nice);
        if (limit < 4) {
            mf_next(f);
            continue;
        }
        const cx_u8* cur = f->base + f->pos;
        HASHES(cur)
        mf_ahead(f, cur, limit);
        cx_u32 cand = f->head4[h4];
        f->head2[h2] = f->head3[h3] = f->head4[h4] = f->pos;
        if (f->bt) bt_skip(f, cand, limit);
        else f->son[f->pos & (f->size - 1)] = cand;
        mf_next(f);
    }
}

/* --- the match finder on all the processors -------------------------------------------------- */

/* The binary trees take most of the time for presets 4 to 9, and they can be split among the
 * threads: each tree holds only the positions with its four-byte hash, which go in in order, so a
 * thread can walk the trees of a share of the hashes over the whole text on its own. Each notes the
 * matches it finds, and then slices of the text are parsed at once from what they noted. The
 * matches are the ones one thread would find. The two- and three-byte matches are found as each
 * slice is parsed, from the text just before it on.
 *
 * A thread's notes are, for each position in its share, in order: how many matches, in a byte, or
 * 255 and then the count in two; then each match in `wide` bytes, 4 or 5, little-endian: its
 * distance less one in the low `dist_bits` bits, and its length less 4 above them. The noted
 * matches are at least 4 long and at most the nice length, and nearer than the dictionary, so at
 * preset 6 they take 4 bytes where a distance and a length would take 6. */
#define RANGES_MAX 254
#define NO_RANGE 255

/* How many bits `v` takes. */
static inline cx_u32 bits_for(cx_u32 v) {
    return v ? 32 - (cx_u32)__builtin_clz(v) : 0;
}

/* Where the next position's matches start, after reading its count from the notes at `q`. */
static inline const cx_u8* note_count(const cx_u8* q, cx_u32* n) {
    cx_u32 c = q[0];
    if (c != 255) {
        *n = c;
        return q + 1;
    }
    cx_u16 c16;
    memcpy(&c16, q + 1, 2);
    *n = c16;
    return q + 3;
}

struct window_s;

typedef struct {
    const cx_u8* data;
    cx_usize len, pos;
    cx_u32 size, reach, nice, dist_bits, wide, ranges;
    /* The window's positions' shares, from `a` to `b`, and where each share's notes are read next.
     * .lzma's one parse goes on from window to window in a ring of WINDOWS, `at` the one it's in. */
    const cx_u8* owner;
    cx_usize a, b;
    struct window_s* ring;
    cx_u32 at;
    const cx_u8* cur[RANGES_MAX];
    cx_u32 head2[HEAD2];
    cx_u32 head3[HEAD3];
} reader_t;

static void rd_turn(reader_t* r);

/* Takes the position at `p` into the two- and three-byte hashes, if it has four bytes. */
static inline int rd_heads(reader_t* r, cx_usize p, cx_u32* d2, cx_u32* d3) {
    if (r->len - p < 4 || r->nice < 4) return 0;
    const cx_u8* cur = r->data + p;
    cx_u32 t = hash_table[cur[0]] ^ cur[1];
    cx_u32 h2 = t & (HEAD2 - 1), h3 = (t ^ ((cx_u32)cur[2] << 8)) & (HEAD3 - 1);
    cx_u32 mp = (cx_u32)p + r->size;
    *d2 = mp - r->head2[h2];
    *d3 = mp - r->head3[h3];
    r->head2[h2] = r->head3[h3] = mp;
    return 1;
}

/* As mf_find, from the notes. */
static cx_u32 rd_find(reader_t* r, match_t* m) {
    const cx_usize p = r->pos++;
    cx_u32 d2, d3;
    if (!rd_heads(r, p, &d2, &d3)) return 0;
    const cx_usize left = r->len - p;
    const cx_u32 avail = left < MATCH_MAX ? (cx_u32)left : MATCH_MAX;
    const cx_u32 limit = avail < r->nice ? avail : r->nice;
    const cx_u8* cur = r->data + p;
    while (p >= r->b) rd_turn(r);
    const cx_u8** at = &r->cur[r->owner[p - r->a]];
    cx_u32 noted;
    const cx_u8* q = note_count(*at, &noted);
    const cx_u32 wide = r->wide, dist_bits = r->dist_bits;
    *at = q + wide * (cx_usize)noted;
    const cx_u32 short_reach = r->reach < SHORT_REACH ? r->reach : SHORT_REACH;
    cx_u32 best = 1, count = 0;
    if (d2 <= short_reach && cur[-(ptrdiff_t)d2] == cur[0]) {
        best = 2;
        m[0].len = 2;
        m[0].dist = d2 - 1;
        count = 1;
    }
    if (d2 != d3 && d3 <= short_reach && cur[-(ptrdiff_t)d3] == cur[0]) {
        best = 3;
        m[count].dist = d3 - 1;
        count++;
        d2 = d3;
    }
    if (count) {
        best = common(cur - d2, cur, best, limit);
        m[count - 1].len = best;
        if (best == limit) goto done;
    }
    if (best < 3) best = 3;
    const cx_u64 dist_mask = ((cx_u64)1 << dist_bits) - 1;
    for (cx_u32 i = 0; i < noted; i++, q += wide) {
        cx_u64 v;
        if (wide == 4) {
            cx_u32 v32;
            memcpy(&v32, q, 4);
            v = v32;
        } else {
            v = 0;
            memcpy(&v, q, 5);
        }
        const cx_u32 dist = (cx_u32)(v & dist_mask), len = (cx_u32)(v >> dist_bits) + 4;
        if (len > best) {
            best = len;
            m[count].len = len;
            m[count].dist = dist;
            count++;
        }
    }
done:
    if (count && m[count - 1].len == r->nice && avail > r->nice) {
        m[count - 1].len = common(cur - m[count - 1].dist - 1, cur, r->nice, avail);
    }
    return count;
}

/* As mf_skip, from the notes. */
static void rd_skip(reader_t* r, cx_u32 n) {
    while (n--) {
        const cx_usize p = r->pos++;
        cx_u32 d2, d3;
        if (!rd_heads(r, p, &d2, &d3)) continue;
        while (p >= r->b) rd_turn(r);
        const cx_u8** at = &r->cur[r->owner[p - r->a]];
        cx_u32 noted;
        const cx_u8* q = note_count(*at, &noted);
        *at = q + r->wide * (cx_usize)noted;
    }
}

/* --- the encoder ----------------------------------------------------------------------------- */

typedef struct {
    prob choice, choice2;
    prob low[POS_STATES_MAX][8];
    prob mid[POS_STATES_MAX][8];
    prob high[256];
} len_probs;

typedef struct {
    cx_u32 prices[POS_STATES_MAX][LEN_SYMBOLS];
    cx_u32 counters[POS_STATES_MAX];
} len_prices;

/* A step of the optimal parse: the cheapest way found to reach this far, where it came from and
 * what it took, and the coder's state and distances there. A step can stand for two or three
 * symbols, a literal and a repeat after a match or a repeat, so the parse finds those too. */
typedef struct {
    cx_u32 price, pos_prev, back_prev;
    cx_u32 pos_prev2, back_prev2;
    cx_u8 prev1_literal, prev2, state;
    cx_u32 reps[REPS];
} opt_t;

/* The coder's settings: the dictionary, lc, lp and pb, the parser, how long a match is long
 * enough, the match finder and how deep it looks. */
typedef struct {
    cx_u32 dict, lc, lp, pb, fast, nice, bt, depth;
} params_t;

typedef struct {
    prob is_match[STATES][POS_STATES_MAX];
    prob is_rep[STATES], is_rep_g0[STATES], is_rep_g1[STATES], is_rep_g2[STATES];
    prob is_rep0_long[STATES][POS_STATES_MAX];
    prob dist_slot[DIST_STATES][64];
    prob dist_special[FULL_DISTANCES - END_POS_MODEL];
    prob align[1 << ALIGN_BITS];
    len_probs match_len, rep_len;
    prob literal[0x300 << 4];
    cx_u32 state, reps[REPS];
    cx_u32 lc, lp, pb_mask, lp_mask;
    cx_u32 fast, nice;
    rc_t rc;
    /* What each symbol costs, worked out again as the probabilities move. */
    len_prices match_prices, rep_prices;
    cx_u32 slot_prices[DIST_STATES][64];
    cx_u32 dist_prices[DIST_STATES][FULL_DISTANCES];
    cx_u32 align_prices[1 << ALIGN_BITS];
    cx_u32 match_count, align_count;
    mf_t mf;
    /* Where the matches come from instead, when threads found them. */
    reader_t* rd;
    /* The block's text, which positions count from. */
    const cx_u8* data;
    /* The matches at the parse's position, and whether they're already found. */
    match_t matches[MATCH_MAX + 2];
    cx_u32 count;
    int ahead;
    cx_u32 opt_cur, opt_end;
    opt_t opts[OPTS];
} enc_t;

static void probs_reset(enc_t* e) {
    prob* p = &e->is_match[0][0];
    prob* end = e->literal + (0x300u << (e->lc + e->lp));
    while (p < end) *p++ = ONE / 2;
    e->state = 0;
    e->reps[0] = e->reps[1] = e->reps[2] = e->reps[3] = 0;
}

static inline prob* lit_probs(enc_t* e, cx_usize pos, cx_u32 prev) {
    cx_u32 k = (((cx_u32)pos & e->lp_mask) << e->lc) + (prev >> (8 - e->lc));
    return e->literal + 0x300 * k;
}

static cx_u32 lit_price(const prob* probs, cx_u32 sym) {
    cx_u32 price = 0, m = 1;
    for (int i = 7; i >= 0; i--) {
        cx_u32 b = (sym >> i) & 1;
        price += PB(probs[m], b);
        m = (m << 1) | b;
    }
    return price;
}

/* A literal after a match, whose bits are guided by the byte the last distance points at, `match`,
 * until one differs. */
static cx_u32 lit_price_matched(const prob* probs, cx_u32 match, cx_u32 sym) {
    cx_u32 price = 0, m = 1, offs = 0x100;
    for (int i = 7; i >= 0; i--) {
        cx_u32 b = (sym >> i) & 1, mb = (match >> i) & 1;
        price += PB(probs[offs + (offs & (mb << 8)) + m], b);
        m = (m << 1) | b;
        offs &= 0u - (b ^ mb ^ 1);
    }
    return price;
}

static inline cx_u32 lit_price_at(enc_t* e, cx_u32 state, const cx_u8* cur, cx_usize pos, cx_u32 match) {
    const prob* probs = lit_probs(e, pos, cur[-1]);
    return state < 7 ? lit_price(probs, cur[0]) : lit_price_matched(probs, match, cur[0]);
}

static void len_prices_update(const len_probs* lp, len_prices* t, cx_u32 ps) {
    cx_u32 a0 = P0(lp->choice), a1 = P1(lp->choice);
    cx_u32* out = t->prices[ps];
    tree_prices(lp->low[ps], 3, a0, out);
    tree_prices(lp->mid[ps], 3, a1 + P0(lp->choice2), out + 8);
    tree_prices(lp->high, 8, a1 + P1(lp->choice2), out + 16);
    t->counters[ps] = LEN_SYMBOLS / 4;
}

static void dist_prices_update(enc_t* e) {
    for (cx_u32 ls = 0; ls < DIST_STATES; ls++) {
        cx_u32* sp = e->slot_prices[ls];
        tree_prices(e->dist_slot[ls], 6, 0, sp);
        for (cx_u32 slot = END_POS_MODEL; slot < 64; slot++) sp[slot] += (((slot >> 1) - 1) - ALIGN_BITS) << 4;
        for (cx_u32 d = 0; d < 4; d++) e->dist_prices[ls][d] = sp[d];
    }
    for (cx_u32 d = 4; d < FULL_DISTANCES; d++) {
        cx_u32 top = 31 - (cx_u32)__builtin_clz(d);
        cx_u32 slot = (top << 1) | ((d >> (top - 1)) & 1);
        cx_u32 footer = (slot >> 1) - 1, base = (2 | (slot & 1)) << footer;
        cx_u32 pr = rtree_price(e->dist_special + base - slot - 1, footer, d - base);
        for (cx_u32 ls = 0; ls < DIST_STATES; ls++) e->dist_prices[ls][d] = e->slot_prices[ls][slot] + pr;
    }
    e->match_count = 0;
}

static void align_prices_update(enc_t* e) {
    for (cx_u32 i = 0; i < (1u << ALIGN_BITS); i++) e->align_prices[i] = rtree_price(e->align, ALIGN_BITS, i);
    e->align_count = 0;
}

static void prices_reset(enc_t* e) {
    if (e->fast) return;
    for (cx_u32 ps = 0; ps <= e->pb_mask; ps++) {
        len_prices_update(&e->match_len, &e->match_prices, ps);
        len_prices_update(&e->rep_len, &e->rep_prices, ps);
    }
    dist_prices_update(e);
    align_prices_update(e);
}

static inline cx_u32 dist_slot(cx_u32 d) {
    if (d < 4) return d;
    cx_u32 top = 31 - (cx_u32)__builtin_clz(d);
    return (top << 1) | ((d >> (top - 1)) & 1);
}

/* A match of `len` at `dist`, past its is_match and is_rep bits. */
static inline cx_u32 match_price(const enc_t* e, cx_u32 dist, cx_u32 len, cx_u32 ps) {
    cx_u32 ls = len - MATCH_MIN < DIST_STATES ? len - MATCH_MIN : DIST_STATES - 1;
    cx_u32 price = e->match_prices.prices[ps][len - MATCH_MIN];
    if (dist < FULL_DISTANCES) return price + e->dist_prices[ls][dist];
    return price + e->slot_prices[ls][dist_slot(dist)] + e->align_prices[dist & ((1u << ALIGN_BITS) - 1)];
}

/* Which of the last distances a repeat uses, past its is_match and is_rep bits and its length. */
static inline cx_u32 rep_price(const enc_t* e, cx_u32 rep, cx_u32 state, cx_u32 ps) {
    if (rep == 0) return P0(e->is_rep_g0[state]) + P1(e->is_rep0_long[state][ps]);
    cx_u32 price = P1(e->is_rep_g0[state]);
    if (rep == 1) return price + P0(e->is_rep_g1[state]);
    return price + P1(e->is_rep_g1[state]) + PB(e->is_rep_g2[state], rep - 2);
}

static inline cx_u32 short_rep_price(const enc_t* e, cx_u32 state, cx_u32 ps) {
    return P0(e->is_rep_g0[state]) + P0(e->is_rep0_long[state][ps]);
}

static inline cx_u32 rep0_len_price(const enc_t* e, cx_u32 len, cx_u32 state, cx_u32 ps) {
    return rep_price(e, 0, state, ps) + e->rep_prices.prices[ps][len - MATCH_MIN];
}

static void enc_len(enc_t* e, len_probs* lp, len_prices* t, cx_u32 sym, cx_u32 ps) {
    rc_t* r = &e->rc;
    if (sym < 8) {
        rc_bit(r, &lp->choice, 0);
        rc_tree(r, lp->low[ps], 3, sym);
    } else if (sym < 16) {
        rc_bit(r, &lp->choice, 1);
        rc_bit(r, &lp->choice2, 0);
        rc_tree(r, lp->mid[ps], 3, sym - 8);
    } else {
        rc_bit(r, &lp->choice, 1);
        rc_bit(r, &lp->choice2, 1);
        rc_tree(r, lp->high, 8, sym - 16);
    }
    if (!e->fast && --t->counters[ps] == 0) len_prices_update(lp, t, ps);
}

/* Codes the symbol the parser chose at `pos`: a literal, a repeat of distance `back` among the
 * last four, or a match at distance back - REPS, `len` long. */
static void encode(enc_t* e, cx_usize pos, cx_u32 len, cx_u32 back) {
    rc_t* r = &e->rc;
    const cx_u32 st = e->state, ps = (cx_u32)pos & e->pb_mask;
    const cx_u8* cur = e->data + pos;
    if (back == LITERAL) {
        rc_bit(r, &e->is_match[st][ps], 0);
        prob* probs = lit_probs(e, pos, pos ? cur[-1] : 0);
        cx_u32 sym = cur[0];
        if (st < 7) {
            rc_tree(r, probs, 8, sym);
        } else {
            cx_u32 match = cur[-(ptrdiff_t)e->reps[0] - 1], m = 1, offs = 0x100;
            for (int i = 7; i >= 0; i--) {
                cx_u32 b = (sym >> i) & 1, mb = (match >> i) & 1;
                rc_bit(r, &probs[offs + (offs & (mb << 8)) + m], b);
                m = (m << 1) | b;
                offs &= 0u - (b ^ mb ^ 1);
            }
        }
        e->state = LIT_NEXT(st);
        return;
    }
    rc_bit(r, &e->is_match[st][ps], 1);
    if (back < REPS) {
        rc_bit(r, &e->is_rep[st], 1);
        if (back == 0) {
            rc_bit(r, &e->is_rep_g0[st], 0);
            rc_bit(r, &e->is_rep0_long[st][ps], len != 1);
            if (len == 1) {
                e->state = SHORTREP_NEXT(st);
                return;
            }
        } else {
            cx_u32 d = e->reps[back];
            rc_bit(r, &e->is_rep_g0[st], 1);
            if (back == 1) {
                rc_bit(r, &e->is_rep_g1[st], 0);
            } else {
                rc_bit(r, &e->is_rep_g1[st], 1);
                rc_bit(r, &e->is_rep_g2[st], back - 2);
                if (back == 3) e->reps[3] = e->reps[2];
                e->reps[2] = e->reps[1];
            }
            e->reps[1] = e->reps[0];
            e->reps[0] = d;
        }
        enc_len(e, &e->rep_len, &e->rep_prices, len - MATCH_MIN, ps);
        e->state = REP_NEXT(st);
        return;
    }
    cx_u32 dist = back - REPS, sym = len - MATCH_MIN;
    rc_bit(r, &e->is_rep[st], 0);
    e->state = MATCH_NEXT(st);
    enc_len(e, &e->match_len, &e->match_prices, sym, ps);
    cx_u32 ls = sym < DIST_STATES ? sym : DIST_STATES - 1, slot = dist_slot(dist);
    rc_tree(r, e->dist_slot[ls], 6, slot);
    if (slot >= 4) {
        cx_u32 footer = (slot >> 1) - 1, base = (2 | (slot & 1)) << footer, reduced = dist - base;
        if (slot < END_POS_MODEL) {
            rc_rtree(r, e->dist_special + base - slot - 1, footer, reduced);
        } else {
            rc_direct(r, reduced >> ALIGN_BITS, footer - ALIGN_BITS);
            rc_rtree(r, e->align, ALIGN_BITS, reduced & ((1u << ALIGN_BITS) - 1));
            e->align_count++;
        }
    }
    e->reps[3] = e->reps[2];
    e->reps[2] = e->reps[1];
    e->reps[1] = e->reps[0];
    e->reps[0] = dist;
    e->match_count++;
}

/* --- the parsers ----------------------------------------------------------------------------- */

static inline cx_u32 find_here(enc_t* e) { return e->rd ? rd_find(e->rd, e->matches) : mf_find(&e->mf, e->matches); }

static inline void skip_here(enc_t* e, cx_u32 n) {
    if (e->rd) rd_skip(e->rd, n);
    else mf_skip(&e->mf, n);
}

/* The matches at the parse's position, found now unless they were already, cut to `avail` bytes:
 * how many. */
static cx_u32 matches_here(enc_t* e, cx_u32 avail) {
    cx_u32 n;
    if (e->ahead) {
        e->ahead = 0;
        n = e->count;
    } else {
        n = find_here(e);
    }
    if (n && e->matches[n - 1].len > avail) {
        if (avail < MATCH_MIN) {
            n = 0;
        } else {
            cx_u32 k = 0;
            while (e->matches[k].len < avail) k++;
            e->matches[k].len = avail;
            n = k + 1;
        }
    }
    e->count = n;
    return n;
}

/* The longest a repeat of each of the last four distances runs at `cur`, up to `limit`, into
 * lens; which is longest. */
static inline cx_u32 rep_lens(const enc_t* e, const cx_u8* cur, cx_u32 limit, cx_u32* lens) {
    cx_u32 best = 0;
    for (cx_u32 i = 0; i < REPS; i++) {
        const cx_u8* b = cur - e->reps[i] - 1;
        lens[i] = load16(cur) == load16(b) ? common(cur, b, 2, limit) : 0;
        if (lens[i] > lens[best]) best = i;
    }
    return best;
}

/* The fast parser, for presets 0 to 3: the longest match, unless a repeat is nearly as long, or a
 * shorter one is much nearer, or the next byte starts a better one. Gives the length and sets
 * *back. */
#define CHANGE_PAIR(small, big) (((big) >> 7) > (small))

static cx_u32 parse_fast(enc_t* e, cx_usize pos, cx_u32 avail, cx_u32* back) {
    cx_u32 n = matches_here(e, avail < MATCH_MAX ? avail : MATCH_MAX);
    if (avail < MATCH_MIN || pos == 0) {
        *back = LITERAL;
        return 1;
    }
    const cx_u8* cur = e->data + pos;
    cx_u32 limit = avail < MATCH_MAX ? avail : MATCH_MAX;
    cx_u32 lens[REPS];
    cx_u32 rep = rep_lens(e, cur, limit, lens), rep_len = lens[rep];
    if (rep_len >= e->nice) {
        skip_here(e, rep_len - 1);
        *back = rep;
        return rep_len;
    }
    cx_u32 len_main = 0, back_main = 0;
    if (n) {
        len_main = e->matches[n - 1].len;
        back_main = e->matches[n - 1].dist;
        if (len_main >= e->nice) {
            skip_here(e, len_main - 1);
            *back = back_main + REPS;
            return len_main;
        }
        while (n > 1 && len_main == e->matches[n - 2].len + 1) {
            if (!CHANGE_PAIR(e->matches[n - 2].dist, back_main)) break;
            n--;
            len_main = e->matches[n - 1].len;
            back_main = e->matches[n - 1].dist;
        }
        if (len_main == 2 && back_main >= 0x80) len_main = 1;
    }
    if (rep_len >= 2 && (rep_len + 1 >= len_main || (rep_len + 2 >= len_main && back_main > (1u << 9)) ||
                         (rep_len + 3 >= len_main && back_main > (1u << 15)))) {
        skip_here(e, rep_len - 1);
        *back = rep;
        return rep_len;
    }
    if (len_main < 2 || avail <= 2) {
        *back = LITERAL;
        return 1;
    }
    /* The next position's matches, which stay found if this is a literal. */
    cx_u32 n2 = matches_here(e, avail - 1 < MATCH_MAX ? avail - 1 : MATCH_MAX);
    e->ahead = 1;
    if (n2) {
        cx_u32 nl = e->matches[n2 - 1].len, nb = e->matches[n2 - 1].dist;
        if ((nl >= len_main && nb < back_main) || (nl == len_main + 1 && !CHANGE_PAIR(back_main, nb)) || nl > len_main + 1 ||
            (nl + 1 >= len_main && len_main >= 3 && CHANGE_PAIR(nb, back_main))) {
            *back = LITERAL;
            return 1;
        }
    }
    cx_u32 lim = len_main - 1 > 2 ? len_main - 1 : 2;
    for (cx_u32 i = 0; i < REPS; i++) {
        if (memcmp(cur + 1, cur - e->reps[i], lim) == 0) {
            *back = LITERAL;
            return 1;
        }
    }
    e->ahead = 0;
    skip_here(e, len_main - 2);
    *back = back_main + REPS;
    return len_main;
}

/* Turns the cheapest path to `cur` around, so the steps can be given from the start, and gives the
 * first. */
static cx_u32 backward(enc_t* e, cx_u32* back, cx_u32 cur) {
    opt_t* o = e->opts;
    e->opt_end = cur;
    cx_u32 pos_mem = o[cur].pos_prev, back_mem = o[cur].back_prev;
    do {
        if (o[cur].prev1_literal) {
            o[pos_mem].back_prev = LITERAL;
            o[pos_mem].prev1_literal = 0;
            o[pos_mem].pos_prev = pos_mem - 1;
            if (o[cur].prev2) {
                o[pos_mem - 1].prev1_literal = 0;
                o[pos_mem - 1].pos_prev = o[cur].pos_prev2;
                o[pos_mem - 1].back_prev = o[cur].back_prev2;
            }
        }
        cx_u32 pp = pos_mem, bc = back_mem;
        back_mem = o[pp].back_prev;
        pos_mem = o[pp].pos_prev;
        o[pp].back_prev = bc;
        o[pp].pos_prev = cur;
        cur = pp;
    } while (cur != 0);
    *back = o[0].back_prev;
    e->opt_cur = o[0].pos_prev;
    return e->opt_cur;
}

#define RELAX(at, cost, from, bk)          \
    do {                                   \
        opt_t* x_ = &o[at];                \
        if ((cost) < x_->price) {          \
            x_->price = (cost);            \
            x_->pos_prev = (from);         \
            x_->back_prev = (bk);          \
            x_->prev1_literal = 0;         \
        }                                  \
    } while (0)

#define RELAX3(at, cost, from, from2, bk2)  \
    do {                                    \
        opt_t* x_ = &o[at];                 \
        if ((cost) < x_->price) {           \
            x_->price = (cost);             \
            x_->pos_prev = (from);          \
            x_->back_prev = 0;              \
            x_->prev1_literal = 1;          \
            x_->prev2 = 1;                  \
            x_->pos_prev2 = (from2);        \
            x_->back_prev2 = (bk2);         \
        }                                   \
    } while (0)

/* The optimal parser, for presets 4 to 9. `avail` is how much text the chunk has left from `pos`,
 * and the parse covers at most `span` bytes of it. Gives the next step's length and sets *back. */
static cx_u32 parse_optimal(enc_t* e, cx_usize pos, cx_u32 avail, cx_u32 span, cx_u32* back) {
    opt_t* o = e->opts;
    if (e->opt_end != e->opt_cur) {
        opt_t* x = &o[e->opt_cur];
        cx_u32 len = x->pos_prev - e->opt_cur;
        *back = x->back_prev;
        e->opt_cur = x->pos_prev;
        return len;
    }
    e->opt_cur = e->opt_end = 0;
    if (e->match_count >= 128) dist_prices_update(e);
    if (e->align_count >= (1u << ALIGN_BITS)) align_prices_update(e);
    if (span > OPTS - 1) span = OPTS - 1;
    if (span < 1) span = 1;

    cx_u32 limit = avail < MATCH_MAX ? avail : MATCH_MAX;
    cx_u32 count = matches_here(e, limit);
    if (avail < MATCH_MIN || pos == 0) {
        *back = LITERAL;
        return 1;
    }
    const cx_u8* cur = e->data + pos;
    cx_u32 lens[REPS];
    cx_u32 rep_max = rep_lens(e, cur, limit, lens);
    if (lens[rep_max] >= e->nice) {
        skip_here(e, lens[rep_max] - 1);
        *back = rep_max;
        return lens[rep_max];
    }
    cx_u32 len_main = count ? e->matches[count - 1].len : 0;
    if (len_main >= e->nice) {
        skip_here(e, len_main - 1);
        *back = e->matches[count - 1].dist + REPS;
        return len_main;
    }
    const cx_u32 state = e->state, ps = (cx_u32)pos & e->pb_mask;
    const cx_u32 cur_byte = cur[0], match_byte = cur[-(ptrdiff_t)e->reps[0] - 1];
    if (len_main < 2 && cur_byte != match_byte && lens[rep_max] < 2) {
        *back = LITERAL;
        return 1;
    }

    o[0].state = (cx_u8)state;
    memcpy(o[0].reps, e->reps, sizeof e->reps);
    o[1].price = P0(e->is_match[state][ps]) + lit_price_at(e, state, cur, pos, match_byte);
    o[1].pos_prev = 0;
    o[1].back_prev = LITERAL;
    o[1].prev1_literal = 0;
    const cx_u32 match_p = P1(e->is_match[state][ps]);
    const cx_u32 rep_match_p = match_p + P1(e->is_rep[state]);
    if (match_byte == cur_byte) {
        cx_u32 sr = rep_match_p + short_rep_price(e, state, ps);
        if (sr < o[1].price) {
            o[1].price = sr;
            o[1].back_prev = 0;
        }
    }
    cx_u32 len_end = len_main > lens[rep_max] ? len_main : lens[rep_max];
    if (len_end < 2) {
        *back = o[1].back_prev;
        return 1;
    }
    for (cx_u32 l = 2; l <= len_end; l++) o[l].price = INF;
    for (cx_u32 i = 0; i < REPS; i++) {
        cx_u32 rl = lens[i];
        if (rl < 2) continue;
        cx_u32 price = rep_match_p + rep_price(e, i, state, ps);
        for (; rl >= 2; rl--) RELAX(rl, price + e->rep_prices.prices[ps][rl - MATCH_MIN], 0, i);
    }
    const cx_u32 normal_p = match_p + P0(e->is_rep[state]);
    cx_u32 l = lens[0] >= 2 ? lens[0] + 1 : 2;
    if (l <= len_main) {
        cx_u32 k = 0;
        while (l > e->matches[k].len) k++;
        for (;; l++) {
            cx_u32 dist = e->matches[k].dist;
            RELAX(l, normal_p + match_price(e, dist, l, ps), 0, dist + REPS);
            if (l == e->matches[k].len && ++k == count) break;
        }
    }

    cx_u32 c = 0;
    for (;;) {
        c++;
        if (c == len_end || c >= span) return backward(e, back, c);
        const cx_usize q = pos + c;
        const cx_u32 left = avail - c;
        cx_u32 n = matches_here(e, left < MATCH_MAX ? left : MATCH_MAX);
        cx_u32 new_len = n ? e->matches[n - 1].len : 0;
        if (new_len >= e->nice) {
            e->ahead = 1;
            return backward(e, back, c);
        }

        /* The state and the distances at c, from the step that reached it. */
        opt_t* oc = &o[c];
        cx_u32 pp = oc->pos_prev, st;
        if (oc->prev1_literal) {
            pp--;
            if (oc->prev2) {
                st = o[oc->pos_prev2].state;
                st = oc->back_prev2 < REPS ? REP_NEXT(st) : MATCH_NEXT(st);
            } else {
                st = o[pp].state;
            }
            st = LIT_NEXT(st);
        } else {
            st = o[pp].state;
        }
        cx_u32 reps[REPS];
        if (pp == c - 1) {
            st = oc->back_prev == 0 ? SHORTREP_NEXT(st) : LIT_NEXT(st);
            memcpy(reps, o[pp].reps, sizeof reps);
        } else {
            cx_u32 bk;
            if (oc->prev1_literal && oc->prev2) {
                pp = oc->pos_prev2;
                bk = oc->back_prev2;
                st = REP_NEXT(st);
            } else {
                bk = oc->back_prev;
                st = bk < REPS ? REP_NEXT(st) : MATCH_NEXT(st);
            }
            const cx_u32* pr = o[pp].reps;
            if (bk < REPS) {
                reps[0] = pr[bk];
                for (cx_u32 i = 0, j = 1; i < REPS; i++) {
                    if (i != bk) reps[j++] = pr[i];
                }
            } else {
                reps[0] = bk - REPS;
                reps[1] = pr[0];
                reps[2] = pr[1];
                reps[3] = pr[2];
            }
        }
        oc->state = (cx_u8)st;
        memcpy(oc->reps, reps, sizeof reps);

        const cx_u8* cq = e->data + q;
        const cx_u32 cur_price = oc->price, cb = cq[0], mb = cq[-(ptrdiff_t)reps[0] - 1];
        const cx_u32 qs = (cx_u32)q & e->pb_mask;
        const cx_u32 lit_p = cur_price + P0(e->is_match[st][qs]) + lit_price_at(e, st, cq, q, mb);
        opt_t* nx = &o[c + 1];
        int next_is_lit = 0;
        if (lit_p < nx->price) {
            nx->price = lit_p;
            nx->pos_prev = c;
            nx->back_prev = LITERAL;
            nx->prev1_literal = 0;
            next_is_lit = 1;
        }
        const cx_u32 mp = cur_price + P1(e->is_match[st][qs]);
        const cx_u32 rmp = mp + P1(e->is_rep[st]);
        if (mb == cb && !(nx->pos_prev < c && nx->back_prev == 0)) {
            cx_u32 sr = rmp + short_rep_price(e, st, qs);
            if (sr <= nx->price) {
                nx->price = sr;
                nx->pos_prev = c;
                nx->back_prev = 0;
                nx->prev1_literal = 0;
                next_is_lit = 1;
            }
        }
        cx_u32 full = left < OPTS - 1 - c ? left : OPTS - 1 - c;
        if (full < 2) continue;
        const cx_u32 lim = full < e->nice ? full : e->nice;

        /* A literal, then a repeat of the last distance. */
        if (!next_is_lit && mb != cb) {
            const cx_u8* b = cq - reps[0] - 1;
            cx_u32 l2lim = full < e->nice + 1 ? full : e->nice + 1;
            cx_u32 t = common(cq + 1, b + 1, 0, l2lim - 1);
            if (t >= 2) {
                cx_u32 st2 = LIT_NEXT(st), ps2 = (cx_u32)(q + 1) & e->pb_mask;
                cx_u32 price = lit_p + P1(e->is_match[st2][ps2]) + P1(e->is_rep[st2]) + rep0_len_price(e, t, st2, ps2);
                cx_u32 off = c + 1 + t;
                while (len_end < off) o[++len_end].price = INF;
                opt_t* x = &o[off];
                if (price < x->price) {
                    x->price = price;
                    x->pos_prev = c + 1;
                    x->back_prev = 0;
                    x->prev1_literal = 1;
                    x->prev2 = 0;
                }
            }
        }

        /* Repeats, and each followed by a literal and a repeat of it. */
        cx_u32 start_len = 2;
        for (cx_u32 ri = 0; ri < REPS; ri++) {
            const cx_u8* b = cq - reps[ri] - 1;
            if (load16(cq) != load16(b)) continue;
            cx_u32 lt = common(cq, b, 2, lim);
            while (len_end < c + lt) o[++len_end].price = INF;
            cx_u32 price = rmp + rep_price(e, ri, st, qs);
            for (cx_u32 k = lt; k >= 2; k--) RELAX(c + k, price + e->rep_prices.prices[qs][k - MATCH_MIN], c, ri);
            if (ri == 0) start_len = lt + 1;
            cx_u32 l2lim = full < lt + 1 + e->nice ? full : lt + 1 + e->nice;
            cx_u32 l2 = common(cq, b, lt + 1, l2lim);
            if (l2 >= lt + 3) {
                l2 -= lt + 1;
                cx_u32 st2 = REP_NEXT(st), ps2 = (cx_u32)(q + lt) & e->pb_mask;
                cx_u32 p2 = price + e->rep_prices.prices[qs][lt - MATCH_MIN] + P0(e->is_match[st2][ps2]) +
                            lit_price_matched(lit_probs(e, q + lt, cq[lt - 1]), b[lt], cq[lt]);
                st2 = LIT_NEXT(st2);
                ps2 = (cx_u32)(q + lt + 1) & e->pb_mask;
                p2 += P1(e->is_match[st2][ps2]) + P1(e->is_rep[st2]) + rep0_len_price(e, l2, st2, ps2);
                cx_u32 off = c + lt + 1 + l2;
                while (len_end < off) o[++len_end].price = INF;
                RELAX3(off, p2, c + lt + 1, c, ri);
            }
        }

        /* Matches, and each followed by a literal and a repeat of it. */
        if (new_len > lim) {
            cx_u32 k = 0;
            while (e->matches[k].len < lim) k++;
            e->matches[k].len = lim;
            n = k + 1;
            new_len = lim;
        }
        if (new_len >= start_len) {
            const cx_u32 nmp = mp + P0(e->is_rep[st]);
            while (len_end < c + new_len) o[++len_end].price = INF;
            cx_u32 k = 0;
            while (start_len > e->matches[k].len) k++;
            for (cx_u32 lt = start_len;; lt++) {
                cx_u32 dist = e->matches[k].dist;
                cx_u32 price = nmp + match_price(e, dist, lt, qs);
                RELAX(c + lt, price, c, dist + REPS);
                if (lt == e->matches[k].len) {
                    const cx_u8* b = cq - dist - 1;
                    cx_u32 l2lim = full < lt + 1 + e->nice ? full : lt + 1 + e->nice;
                    cx_u32 l2 = common(cq, b, lt + 1, l2lim);
                    if (l2 >= lt + 3) {
                        l2 -= lt + 1;
                        cx_u32 st2 = MATCH_NEXT(st), ps2 = (cx_u32)(q + lt) & e->pb_mask;
                        cx_u32 p2 = price + P0(e->is_match[st2][ps2]) + lit_price_matched(lit_probs(e, q + lt, cq[lt - 1]), b[lt], cq[lt]);
                        st2 = LIT_NEXT(st2);
                        ps2 = (cx_u32)(q + lt + 1) & e->pb_mask;
                        p2 += P1(e->is_match[st2][ps2]) + P1(e->is_rep[st2]) + rep0_len_price(e, l2, st2, ps2);
                        cx_u32 off = c + lt + 1 + l2;
                        while (len_end < off) o[++len_end].price = INF;
                        RELAX3(off, p2, c + lt + 1, c, dist + REPS);
                    }
                    if (++k == n) break;
                }
            }
        }
    }
}

/* --- slices ---------------------------------------------------------------------------------- */

/* A growing buffer of code, or of notes. It grows by half, as the notes of a window take a lot. */
typedef struct {
    cx_u8* p;
    cx_usize len, cap;
} buf_t;

static void buf_room(buf_t* b, cx_usize more) {
    if (b->cap - b->len >= more) return;
    cx_usize cap = b->cap ? b->cap : 65536;
    while (cap - b->len < more) cap += cap / 2;
    b->p = (cx_u8*)cx_realloc(b->p, b->cap, cap);
    b->cap = cap;
}

/* A slice of a block: its match finder takes in the text from `win`, and it codes start..end. The
 * first slice of a block resets the dictionary. */
typedef struct {
    const cx_u8* data;
    cx_usize len;
    cx_usize win, start, end;
    int first;
    buf_t out;
} slice_t;

/* The memory a slice's coder takes, in its parts. */
typedef struct {
    cx_u32 size, hash_bits;
} mf_shape_t;

static cx_u32 pow2_at_least(cx_u64 v) {
    cx_u32 p = 1u << 12;
    while (p < v && p < (1u << 31)) p <<= 1;
    return p;
}

static mf_shape_t mf_shape(const params_t* pr, cx_u64 window) {
    mf_shape_t s;
    s.size = pow2_at_least(window < pr->dict ? window : pr->dict);
    cx_u32 bits = 31 - (cx_u32)__builtin_clz(s.size);
    s.hash_bits = bits - 1 < 10 ? 10 : bits - 1 > 24 ? 24 : bits - 1;
    return s;
}

static cx_usize mf_bytes(const params_t* pr, mf_shape_t s) {
    return ((cx_usize)HEAD2 + HEAD3 + ((cx_usize)1 << s.hash_bits) + ((cx_usize)s.size << pr->bt)) * sizeof(cx_u32);
}

/* A coder with room for a match finder of shape `s`. */
typedef struct {
    enc_t* e;
    cx_u32* tables;
    cx_usize bytes;
    mf_shape_t shape;
} coder_t;

static void coder_free(coder_t* c) {
    if (c->e) cx_free(c->e, sizeof(enc_t));
    if (c->tables) cx_free(c->tables, c->bytes);
    c->e = 0;
    c->tables = 0;
}

/* Asks Linux to back the tables at p with 2 MiB pages, which most systems give only when asked. A
 * search's steps land all over tens of MiB of trees, and with 4 KiB pages most of them miss the
 * TLB as well as the cache. Windows gives large pages only to accounts allowed to lock memory. */
static void big_pages(void* p, cx_usize n) {
#ifdef MADV_HUGEPAGE
    const uintptr_t big = (uintptr_t)2 << 20;
    uintptr_t from = ((uintptr_t)p + big - 1) & ~(big - 1), to = ((uintptr_t)p + n) & ~(big - 1);
    if (to > from) madvise((void*)from, to - from, MADV_HUGEPAGE);
#else
    (void)p;
    (void)n;
#endif
}

static void coder_ready(coder_t* c, const params_t* pr, mf_shape_t s) {
    if (!c->e) c->e = (enc_t*)cx_alloc(sizeof(enc_t));
    cx_usize bytes = mf_bytes(pr, s);
    if (c->tables && (c->shape.size < s.size || c->shape.hash_bits != s.hash_bits)) {
        cx_free(c->tables, c->bytes);
        c->tables = 0;
    }
    if (!c->tables) {
        c->tables = (cx_u32*)cx_alloc(bytes);
        big_pages(c->tables, bytes);
        c->bytes = bytes;
        c->shape = s;
    }
}

static void enc_setup(enc_t* e, const params_t* pr, const cx_u8* data);

/* Sets the coder up to code text from data + win on, with matches reaching back to win at most
 * and forward to text_end, the end of the block, whose size the match finder takes its shape from,
 * so a slice finds the same matches whatever text before it it took in. */
static void coder_start(coder_t* c, const params_t* pr, const cx_u8* data, cx_usize win, cx_usize text_end) {
    enc_t* e = c->e;
    mf_t* f = &e->mf;
    mf_shape_t s = mf_shape(pr, text_end);
    f->size = s.size;
    f->reach = s.size - 1;
    f->hash_bits = s.hash_bits;
    f->nice = pr->nice;
    f->depth = pr->depth;
    f->bt = (int)pr->bt;
    f->head2 = c->tables;
    f->head3 = f->head2 + HEAD2;
    f->head4 = f->head3 + HEAD3;
    f->son = f->head4 + ((cx_usize)1 << s.hash_bits);
    memset(c->tables, 0, ((cx_usize)HEAD2 + HEAD3 + ((cx_usize)1 << s.hash_bits)) * sizeof(cx_u32));
    /* Position `size` is data[win], so 0 is out of reach. */
    f->pos = s.size;
    f->base = (const cx_u8*)((uintptr_t)(data + win) - s.size);
    f->base_at = (cx_u64)win - s.size;
    f->text_end = text_end;
    enc_setup(e, pr, data);
}

/* Sets the coder's settings and state up, for text at `data`. */
static void enc_setup(enc_t* e, const params_t* pr, const cx_u8* data) {
    e->rd = 0;
    e->data = data;
    e->lc = pr->lc;
    e->lp = pr->lp;
    e->pb_mask = (1u << pr->pb) - 1;
    e->lp_mask = (1u << pr->lp) - 1;
    e->fast = pr->fast;
    e->nice = pr->nice;
    e->ahead = 0;
    e->count = 0;
    e->opt_cur = e->opt_end = 0;
    probs_reset(e);
    prices_reset(e);
}

static void put_chunk_lzma(buf_t* out, cx_u32 reset, cx_usize text, const rc_t* r, cx_u8 props) {
    buf_room(out, 6 + r->n);
    cx_u8* p = out->p + out->len;
    cx_usize u = text - 1, k = r->n - 1;
    p[0] = (cx_u8)(0x80 | (reset << 5) | (u >> 16));
    p[1] = (cx_u8)(u >> 8);
    p[2] = (cx_u8)u;
    p[3] = (cx_u8)(k >> 8);
    p[4] = (cx_u8)k;
    cx_usize head = 5;
    if (reset >= 2) p[head++] = props;
    memcpy(p + head, r->out, r->n);
    out->len += head + r->n;
}

static void put_chunks_raw(buf_t* out, const cx_u8* text, cx_usize n, int dict_reset) {
    while (n) {
        cx_usize k = n < CHUNK_CODE ? n : CHUNK_CODE;
        buf_room(out, 3 + k);
        cx_u8* p = out->p + out->len;
        p[0] = dict_reset ? 1 : 2;
        p[1] = (cx_u8)((k - 1) >> 8);
        p[2] = (cx_u8)(k - 1);
        memcpy(p + 3, text, k);
        out->len += 3 + k;
        text += k;
        n -= k;
        dict_reset = 0;
    }
}

/* Codes the slice's text as LZMA2 chunks, without the end, into its buffer, with the coder set up. */
static void slice_chunks(enc_t* e, const params_t* pr, slice_t* s, cx_u8* scratch) {
    const cx_u8 props = (cx_u8)((pr->pb * 5 + pr->lp) * 9 + pr->lc);
    int dict_reset = s->first, need_props = 1, state_reset = 0;
    cx_usize pos = s->start;
    while (pos < s->end) {
        const cx_usize chunk = pos;
        const cx_usize cap = s->end - pos < CHUNK_TEXT ? s->end : pos + CHUNK_TEXT;
        if (state_reset) {
            probs_reset(e);
            prices_reset(e);
        }
        rc_start(&e->rc, scratch);
        while (pos < cap) {
            cx_usize used = rc_size(&e->rc);
            int queued = e->opt_cur != e->opt_end;
            if (!queued && used + SYMBOL_CODE > CHUNK_CODE) break;
            cx_u32 avail = cap - pos > OPTS ? OPTS : (cx_u32)(cap - pos), back, len;
            if (e->fast) {
                len = parse_fast(e, pos, avail, &back);
            } else {
                cx_usize room = used + SYMBOL_CODE < CHUNK_CODE ? (CHUNK_CODE - SYMBOL_CODE - used) / CODE_PER_BYTE : 0;
                len = parse_optimal(e, pos, avail, (cx_u32)(room < OPTS ? room : OPTS), &back);
            }
            encode(e, pos, len, back);
            pos += len;
        }
        rc_flush(&e->rc);
        cx_usize text = pos - chunk;
        if (e->rc.n <= CHUNK_CODE && e->rc.n + 2 < text) {
            cx_u32 reset = dict_reset ? 3 : need_props ? 2 : state_reset ? 1 : 0;
            put_chunk_lzma(&s->out, reset, text, &e->rc, props);
            dict_reset = need_props = state_reset = 0;
        } else {
            /* It doesn't compress: the text as it is, and a coder starting afresh, as the decoder
             * never saw this one's symbols. */
            put_chunks_raw(&s->out, s->data + chunk, text, dict_reset);
            dict_reset = 0;
            state_reset = 1;
        }
    }
}

/* Codes the slice as LZMA2 chunks with a match finder of its own, which first takes in the text
 * from s->win. */
static void slice_lzma2(coder_t* c, const params_t* pr, slice_t* s, cx_u8* scratch) {
    coder_start(c, pr, s->data, s->win, s->len);
    skip_here(c->e, (cx_u32)(s->start - s->win));
    slice_chunks(c->e, pr, s, scratch);
}

/* Codes a block's slices s[0..n) one after another, with one match finder: the same as coding
 * them at once, each with a match finder of its own or all with the threads'. */
static void block_lzma2(coder_t* c, const params_t* pr, slice_t* s, cx_u64 n, cx_u8* scratch) {
    coder_start(c, pr, s[0].data, 0, s[0].len);
    enc_t* e = c->e;
    for (cx_u64 k = 0; k < n; k++) {
        if (k) {
            probs_reset(e);
            prices_reset(e);
            e->opt_cur = e->opt_end = 0;
        }
        slice_chunks(e, pr, &s[k], scratch);
    }
}

/* .lzma's one LZMA stream, coded into the slice's buffer: started, taken on from *at to `until`, and
 * ended. */
static void lzma1_start(enc_t* e, slice_t* s) {
    buf_room(&s->out, 65536);
    rc_start(&e->rc, s->out.p);
}

static void lzma1_run(enc_t* e, slice_t* s, cx_usize* at, cx_usize until) {
    cx_usize pos = *at;
    while (pos < until) {
        /* Room for the most a parse can take. */
        if (s->out.cap - e->rc.n < (cx_usize)OPTS * CODE_PER_BYTE + 2 * SYMBOL_CODE + e->rc.pending) {
            s->out.len = e->rc.n;
            buf_room(&s->out, (cx_usize)OPTS * CODE_PER_BYTE + 2 * SYMBOL_CODE + e->rc.pending + s->out.cap);
            e->rc.out = s->out.p;
        }
        cx_usize left = s->len - pos;
        cx_u32 avail = left > OPTS ? OPTS : (cx_u32)left, back, len;
        len = e->fast ? parse_fast(e, pos, avail, &back) : parse_optimal(e, pos, avail, OPTS, &back);
        encode(e, pos, len, back);
        pos += len;
    }
    *at = pos;
}

static void lzma1_end(enc_t* e, slice_t* s) {
    rc_flush(&e->rc);
    s->out.len = e->rc.n;
}

/* Codes the slice as one LZMA stream, for .lzma, with a match finder of its own. */
static void slice_lzma1(coder_t* c, const params_t* pr, slice_t* s) {
    coder_start(c, pr, s->data, 0, s->len);
    lzma1_start(c->e, s);
    cx_usize pos = 0;
    lzma1_run(c->e, s, &pos, s->len);
    lzma1_end(c->e, s);
}

/* --- jobs ------------------------------------------------------------------------------------ */

typedef struct {
    params_t pr;
    int alone, shared, alone_shared, per_slice;
    cx_u64 blocks, slices, groups;
    slice_t* s;
    cx_u64* first; /* each block's first slice, and the slice count at the end */
} job_t;

static cx_u64 phys_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) return m.ullTotalPhys;
    return (cx_u64)4 << 30;
#else
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0) return (cx_u64)pages * (cx_u64)size;
    return (cx_u64)4 << 30;
#endif
}

/* Each group's slices, or blocks, one after another, in a coder big enough for the biggest. */
static void job_body(void* v, cx_u64 begin, cx_u64 end) {
    job_t* j = (job_t*)v;
    cx_u8* scratch = j->alone ? 0 : (cx_u8*)cx_alloc(2 * CHUNK_CODE);
    const cx_u64 units = j->per_slice ? j->slices : j->blocks;
    for (cx_u64 g = begin; g < end; g++) {
        coder_t c = {0};
        mf_shape_t big = {0, 0};
        for (cx_u64 k = g; k < units; k += j->groups) {
            const slice_t* s = &j->s[j->per_slice ? k : j->first[k]];
            mf_shape_t sh = mf_shape(&j->pr, s->len);
            if (sh.size > big.size) big = sh;
        }
        for (cx_u64 k = g; k < units; k += j->groups) {
            coder_ready(&c, &j->pr, big);
            if (j->alone) slice_lzma1(&c, &j->pr, &j->s[0]);
            else if (j->per_slice) slice_lzma2(&c, &j->pr, &j->s[k], scratch);
            else block_lzma2(&c, &j->pr, &j->s[j->first[k]], j->first[k + 1] - j->first[k], scratch);
        }
        coder_free(&c);
    }
    if (scratch) cx_free(scratch, 2 * CHUNK_CODE);
}

/* A block's match finder, shared by the threads. */
typedef struct {
    const params_t* pr;
    const cx_u8* data;
    cx_usize len;
    cx_u32 size, reach, hash_bits, ranges, dist_bits, wide;
    cx_u32* head4;
    cx_u32* son;
} shared_t;

/* A window of the block's text, a few slices long: its positions' shares, its slices, and each
 * share's notes, with where each slice's start is in them. */
typedef struct window_s {
    shared_t* f;
    cx_usize a, b;
    cx_u8* owner;
    cx_usize owner_cap;
    slice_t* slices;
    cx_u32 nslices;
    buf_t* notes;
    cx_usize* marks;
    cx_usize marks_cap;
} window_t;

/* .lzma's parse keeps a ring of three windows: the one being found, the one it's parsing, and the
 * one before, which it may still be finishing. */
#define WINDOWS 3

/* Moves the reader on to the next window of its ring, from the start of each share's notes. */
static void rd_turn(reader_t* r) {
    const window_t* w = &r->ring[++r->at % WINDOWS];
    r->owner = w->owner;
    r->a = w->a;
    r->b = w->b;
    for (cx_u32 k = 0; k < r->ranges; k++) r->cur[k] = w->notes[k].p;
}

/* A step: the matches of one window found while the slices of the one before are parsed. */
typedef struct {
    window_t* find;
    window_t* parse;
} step_t;

#define OWNER_CHUNK 65536

static void owner_body(void* v, cx_u64 begin, cx_u64 end) {
    window_t* w = (window_t*)v;
    const shared_t* f = w->f;
    for (cx_u64 c = begin; c < end; c++) {
        cx_usize from = w->a + (cx_usize)c * OWNER_CHUNK, to = from + OWNER_CHUNK < w->b ? from + OWNER_CHUNK : w->b;
        for (cx_usize p = from; p < to; p++) {
            if (f->len - p < 4 || f->pr->nice < 4) {
                w->owner[p - w->a] = NO_RANGE;
                continue;
            }
            cx_u32 h4 = (load32(f->data + p) * 0x9E3779B1u) >> (32 - f->hash_bits);
            w->owner[p - w->a] = (cx_u8)((h4 * f->ranges) >> f->hash_bits);
        }
    }
}

/* Walks the trees of share r over the window, noting the matches. */
static void find_range(window_t* w, cx_u32 r) {
    const shared_t* f = w->f;
    match_t m[MATCH_MAX + 2];
    mf_t view;
    memset(&view, 0, sizeof view);
    view.base = (const cx_u8*)((uintptr_t)f->data - f->size);
    view.size = f->size;
    view.reach = f->reach;
    view.nice = f->pr->nice;
    view.depth = f->pr->depth;
    view.bt = (int)f->pr->bt;
    view.son = f->son;
    const cx_u32 nice = f->pr->nice, mask = f->size - 1, wide = f->wide, dist_bits = f->dist_bits;
    buf_t* o = &w->notes[r];
    o->len = 0;
    cx_u32 k = 0;
    for (cx_usize p = w->a; p < w->b; p++) {
        while (k < w->nslices && w->slices[k].start <= p) w->marks[r * w->nslices + k++] = o->len;
        if (w->owner[p - w->a] != r) continue;
        const cx_usize left = f->len - p;
        const cx_u32 limit = left < nice ? (cx_u32)left : nice;
        const cx_u8* cur = f->data + p;
        cx_u32 h4 = (load32(cur) * 0x9E3779B1u) >> (32 - f->hash_bits);
        cx_u32 mp = (cx_u32)p + f->size;
        cx_u32 cand = f->head4[h4];
        f->head4[h4] = mp;
        view.pos = mp;
        cx_u32 n;
        if (view.bt) {
            n = bt_find(&view, cand, limit, 3, m);
        } else {
            f->son[mp & mask] = cand;
            n = hc_find(&view, cand, limit, 3, m);
        }
        buf_room(o, 3 + (cx_usize)wide * n);
        cx_u8* q = o->p + o->len;
        if (n < 255) {
            *q++ = (cx_u8)n;
        } else {
            const cx_u16 n16 = (cx_u16)n;
            *q = 255;
            memcpy(q + 1, &n16, 2);
            q += 3;
        }
        for (cx_u32 i = 0; i < n; i++, q += wide) {
            const cx_u64 v = (cx_u64)m[i].dist | ((cx_u64)(m[i].len - 4) << dist_bits);
            if (wide == 4) {
                const cx_u32 v32 = (cx_u32)v;
                memcpy(q, &v32, 4);
            } else {
                memcpy(q, &v, 5);
            }
        }
        o->len = (cx_usize)(q - o->p);
    }
    while (k < w->nslices) w->marks[r * w->nslices + k++] = o->len;
}

/* Parses the window's slice k from the notes. */
static void parse_slice(window_t* w, cx_u64 k, enc_t* e, reader_t* rd, cx_u8* scratch) {
    const shared_t* f = w->f;
    slice_t* s = &w->slices[k];
    memset(rd->head2, 0, sizeof rd->head2);
    memset(rd->head3, 0, sizeof rd->head3);
    rd->data = f->data;
    rd->len = f->len;
    rd->size = f->size;
    rd->reach = f->reach;
    rd->nice = f->pr->nice;
    rd->dist_bits = f->dist_bits;
    rd->wide = f->wide;
    rd->ranges = f->ranges;
    rd->owner = w->owner;
    rd->a = w->a;
    rd->b = (cx_usize)-1; /* a slice is parsed within its window */
    for (cx_u32 r = 0; r < f->ranges; r++) rd->cur[r] = w->notes[r].p + w->marks[r * w->nslices + k];
    /* The two- and three-byte hashes from as far before the slice as they reach. */
    cx_usize from = s->start > SHORT_REACH ? s->start - SHORT_REACH : 0;
    for (cx_usize p = from; p < s->start; p++) {
        cx_u32 d2, d3;
        (void)rd_heads(rd, p, &d2, &d3);
    }
    rd->pos = s->start;
    enc_setup(e, f->pr, f->data);
    e->rd = rd;
    slice_chunks(e, f->pr, s, scratch);
}

/* The parses come first, as they take longest. */
static void step_body(void* v, cx_u64 begin, cx_u64 end) {
    step_t* st = (step_t*)v;
    cx_u64 parses = st->parse ? st->parse->nslices : 0;
    enc_t* e = 0;
    reader_t* rd = 0;
    cx_u8* scratch = 0;
    for (cx_u64 i = begin; i < end; i++) {
        if (i < parses) {
            if (!e) {
                e = (enc_t*)cx_alloc(sizeof(enc_t));
                rd = (reader_t*)cx_alloc(sizeof(reader_t));
                scratch = (cx_u8*)cx_alloc(2 * CHUNK_CODE);
            }
            parse_slice(st->parse, i, e, rd, scratch);
        } else {
            find_range(st->find, (cx_u32)(i - parses));
        }
    }
    if (e) {
        cx_free(scratch, 2 * CHUNK_CODE);
        cx_free(rd, sizeof(reader_t));
        cx_free(e, sizeof(enc_t));
    }
}

/* Sets the window up for slices s[0..k), and works out its positions' shares. */
static void window_start(window_t* w, slice_t* s, cx_u64 k) {
    w->a = s[0].start;
    w->b = s[k - 1].end;
    w->slices = s;
    w->nslices = (cx_u32)k;
    if (w->b - w->a > w->owner_cap) {
        if (w->owner) cx_free(w->owner, w->owner_cap);
        w->owner_cap = w->b - w->a;
        w->owner = (cx_u8*)cx_alloc(w->owner_cap);
    }
    if (w->f->ranges * k > w->marks_cap) {
        if (w->marks) cx_free(w->marks, w->marks_cap * sizeof(cx_usize));
        w->marks_cap = w->f->ranges * k;
        w->marks = (cx_usize*)cx_alloc(w->marks_cap * sizeof(cx_usize));
    }
    cx_parallel((w->b - w->a + OWNER_CHUNK - 1) / OWNER_CHUNK, owner_body, w);
}

static void window_free(window_t* w) {
    if (w->owner) cx_free(w->owner, w->owner_cap);
    if (w->marks) cx_free(w->marks, w->marks_cap * sizeof(cx_usize));
    for (cx_u32 r = 0; r < w->f->ranges; r++) {
        if (w->notes[r].p) cx_free(w->notes[r].p, w->notes[r].cap);
    }
    cx_free(w->notes, w->f->ranges * sizeof(buf_t));
}

static cx_usize shared_head_bytes(const shared_t* f) {
    return ((cx_usize)1 << f->hash_bits) * sizeof(cx_u32);
}

static cx_usize shared_son_bytes(const shared_t* f) {
    return ((cx_usize)f->size << f->pr->bt) * sizeof(cx_u32);
}

/* Sets up the trees the threads share, for the text of `len` bytes at data, each of whose windows
 * is at most `window` bytes. The threads walk different positions at once, so when the text is
 * longer than the cyclic buffer, one ahead would write over the nodes of positions another still
 * reads. So the threads all finish a window before the next, and the buffer has room for the
 * dictionary and a window. */
static void shared_start(shared_t* f, const params_t* pr, const cx_u8* data, cx_usize len, cx_usize window) {
    memset(f, 0, sizeof *f);
    f->pr = pr;
    f->data = data;
    f->len = len;
    mf_shape_t sh = mf_shape(pr, len);
    f->size = len > sh.size ? pow2_at_least((cx_u64)sh.size + window) : sh.size;
    f->reach = sh.size - 1;
    f->hash_bits = sh.hash_bits;
    /* A noted match is nearer than the reach and no longer than the nice length. */
    f->dist_bits = bits_for(f->reach);
    f->wide = f->dist_bits + bits_for(pr->nice - 4) > 32 ? 5 : 4;
    cx_usize workers = cx_workers();
    f->ranges = (cx_u32)(workers * 2 < RANGES_MAX ? workers * 2 : RANGES_MAX);
    f->head4 = (cx_u32*)cx_alloc(shared_head_bytes(f));
    big_pages(f->head4, shared_head_bytes(f));
    memset(f->head4, 0, shared_head_bytes(f));
    f->son = (cx_u32*)cx_alloc(shared_son_bytes(f));
    big_pages(f->son, shared_son_bytes(f));
}

static void shared_free(shared_t* f) {
    cx_free(f->son, shared_son_bytes(f));
    cx_free(f->head4, shared_head_bytes(f));
}

/* Windows with notes for each of the shared trees' shares. */
static void windows_start(window_t* w, int n, shared_t* f) {
    memset(w, 0, n * sizeof(window_t));
    for (int t = 0; t < n; t++) {
        w[t].f = f;
        w[t].notes = (buf_t*)cx_alloc(f->ranges * sizeof(buf_t));
        memset(w[t].notes, 0, f->ranges * sizeof(buf_t));
    }
}

/* Compresses the block whose slices are s[0..n) with all the threads finding its matches, a
 * window of slices at a time, each window's matches found while the one before is parsed. */
static void block_shared(const params_t* pr, slice_t* s, cx_u64 n) {
    const mf_shape_t sh = mf_shape(pr, s[0].len);
    const cx_usize first_len = s[0].end - s[0].start + 1; /* the slices differ by a byte at most */
    const cx_usize window_max = s[0].len > sh.size ? (sh.size > first_len ? sh.size : first_len) : (cx_usize)1 << 24;
    shared_t f;
    shared_start(&f, pr, s[0].data, s[0].len, window_max);
    const cx_usize workers = cx_workers();
    window_t w[2];
    windows_start(w, 2, &f);
    /* Windows of as many slices as there are workers, and no more text than window_max. */
    window_t* prev = 0;
    int t = 0;
    for (cx_u64 i = 0; i < n || prev;) {
        window_t* cur = 0;
        if (i < n) {
            cx_u64 k = 1;
            while (i + k < n && k < workers && s[i + k].end - s[i].start <= window_max) k++;
            cur = &w[t];
            t ^= 1;
            window_start(cur, s + i, k);
            i += k;
        }
        step_t st = {cur, prev};
        cx_parallel((prev ? prev->nslices : 0) + (cur ? f.ranges : 0), step_body, &st);
        prev = cur;
    }
    window_free(&w[0]);
    window_free(&w[1]);
    shared_free(&f);
}

/* .lzma's windows: small, as its one parse can't start on a window until it's found, and the
 * threads find one in a few milliseconds while the parse takes about a tenth of a second. */
#define ALONE_WINDOW ((cx_usize)1 << 20)
/* How far past a fresh parse's position it may read matches: the most it looks ahead, and a
 * match's length past that. */
#define PARSE_REACH ((cx_usize)OPTS + MATCH_MAX + 1)

/* A step of .lzma: the matches of one window found, and the parse taken on to `until`. */
typedef struct {
    window_t* find;
    int parse;
    enc_t* e;
    slice_t* s;
    cx_usize* pos;
    cx_usize until;
} alone_step_t;

static void alone_body(void* v, cx_u64 begin, cx_u64 end) {
    alone_step_t* st = (alone_step_t*)v;
    for (cx_u64 i = begin; i < end; i++) {
        if (st->parse && i == 0) lzma1_run(st->e, st->s, st->pos, st->until);
        else find_range(st->find, (cx_u32)(i - (cx_u64)st->parse));
    }
}

/* Compresses .lzma's one stream with all the threads finding its matches, a window at a time, and
 * one parsing it from their notes. While the threads find a window, the parse goes on through the
 * ones before, up to as far before the last one found as it may read on. It finds the same matches
 * as one thread, so it writes the same bytes. */
static void alone_shared(const params_t* pr, slice_t* s) {
    shared_t f;
    shared_start(&f, pr, s->data, s->len, ALONE_WINDOW);
    const cx_usize nw = (s->len + ALONE_WINDOW - 1) / ALONE_WINDOW;
    slice_t* ws = (slice_t*)cx_alloc(nw * sizeof(slice_t));
    memset(ws, 0, nw * sizeof(slice_t));
    for (cx_usize k = 0; k < nw; k++) {
        ws[k].start = k * ALONE_WINDOW;
        ws[k].end = k + 1 < nw ? (k + 1) * ALONE_WINDOW : s->len;
    }
    window_t ring[WINDOWS];
    windows_start(ring, WINDOWS, &f);
    enc_t* e = (enc_t*)cx_alloc(sizeof(enc_t));
    reader_t* rd = (reader_t*)cx_alloc(sizeof(reader_t));
    memset(rd->head2, 0, sizeof rd->head2);
    memset(rd->head3, 0, sizeof rd->head3);
    rd->data = f.data;
    rd->len = f.len;
    rd->pos = 0;
    rd->size = f.size;
    rd->reach = f.reach;
    rd->nice = pr->nice;
    rd->dist_bits = f.dist_bits;
    rd->wide = f.wide;
    rd->ranges = f.ranges;
    rd->ring = ring;
    rd->at = (cx_u32)-1; /* the first position it reads turns it to ring[0] */
    rd->a = rd->b = 0;
    enc_setup(e, pr, f.data);
    e->rd = rd;
    lzma1_start(e, s);
    cx_usize pos = 0;
    for (cx_usize k = 0; k <= nw; k++) {
        window_t* find = 0;
        if (k < nw) {
            find = &ring[k % WINDOWS];
            window_start(find, &ws[k], 1);
        }
        alone_step_t st = {find, k > 0, e, s, &pos, 0};
        if (k == nw) st.until = s->len;
        else if (k > 0) st.until = ws[k - 1].end > PARSE_REACH ? ws[k - 1].end - PARSE_REACH : 0;
        cx_parallel((cx_u64)st.parse + (find ? f.ranges : 0), alone_body, &st);
    }
    lzma1_end(e, s);
    cx_free(rd, sizeof(reader_t));
    cx_free(e, sizeof(enc_t));
    for (int t = 0; t < WINDOWS; t++) window_free(&ring[t]);
    cx_free(ws, nw * sizeof(slice_t));
    shared_free(&f);
}

/* opt: the dictionary size, lc, lp, pb, whether the parser is the fast one, the nice length,
 * whether the match finder is a binary tree, its depth (0 for its own), the block size, the slice
 * size (0 for its own), how far back a slice's matches reach before it (0 for the dictionary),
 * and whether it's .lzma. */
cx_u64 cx_xze_start(const cx_u8* in, cx_u64 n, const cx_u64* opt, cx_u64* sizes) {
    if (!__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE)) tables_init();
    job_t* j = (job_t*)cx_alloc(sizeof(job_t));
    memset(j, 0, sizeof *j);
    params_t* pr = &j->pr;
    pr->dict = (cx_u32)opt[0];
    pr->lc = (cx_u32)opt[1];
    pr->lp = (cx_u32)opt[2];
    pr->pb = (cx_u32)opt[3];
    pr->fast = (cx_u32)opt[4];
    pr->nice = (cx_u32)opt[5];
    pr->bt = (cx_u32)opt[6];
    pr->depth = (cx_u32)opt[7];
    if (pr->depth == 0) pr->depth = pr->bt ? 16 + pr->nice / 2 : 4 + pr->nice / 4;
    j->alone = (int)opt[11];
    cx_u64 block = opt[8], slice = opt[9], reach = opt[10] ? opt[10] : pr->dict;
    cx_usize workers = cx_workers();
    if (j->alone) {
        j->blocks = 1;
        block = n;
    } else {
        if (block == 0) block = n ? n : 1;
        j->blocks = n / block + (n % block != 0);
    }
    /* Each block is cut into slices of `slice` bytes, each starting with a state reset, whatever
     * the processors, so the same bytes always compress to the same. How they're compressed
     * depends on the processors. With fewer blocks than workers, presets 0 to 3 give each slice a
     * match finder of its own, which takes in the dictionary before it, as hash chains take text in
     * quickly; and presets 4 to 9 have all the threads find a block's matches, a block at a time,
     * when there's one block or the blocks are longer than a slice. Otherwise the blocks compress
     * at once, each on a thread. A slice is never past 1 GiB, which keeps positions in 32 bits. */
    const cx_u64 slice_max = (cx_u64)1 << 30;
    if (slice == 0) slice = (cx_u64)1 << 20;
    j->shared = !j->alone && pr->bt && workers > 1 && j->blocks < workers && n >= ((cx_u64)1 << 18) && block <= ((cx_u64)1 << 31) && (j->blocks == 1 || block > slice);
    if (j->shared) {
        /* All the threads finding a block's matches takes the trees, with room for the dictionary
         * and a window more, and the notes of two windows, about 12 bytes a position: as
         * block_shared sizes them. Without half the memory for that, each block compresses on a
         * thread, which writes the same bytes. */
        const cx_u64 len = block < n ? block : n;
        const mf_shape_t sh = mf_shape(pr, len);
        const int wraps = len > sh.size;
        const cx_u64 window = wraps ? sh.size : len < ((cx_u64)1 << 24) ? len : (cx_u64)1 << 24;
        const cx_u64 trees = (wraps ? 2 * (cx_u64)sh.size : (cx_u64)sh.size) * 2 * sizeof(cx_u32);
        const cx_u64 need = trees + ((cx_u64)sizeof(cx_u32) << sh.hash_bits) + 2 * 12 * window;
        if (need > phys_memory() / 2) j->shared = 0;
    }
    /* .lzma is one stream, parsed on one thread, but the threads can still find its matches, with
     * trees of room for the dictionary and a window of 1 MiB more, and three windows' notes. */
    j->alone_shared = j->alone && pr->bt && workers > 1 && n >= ((cx_u64)1 << 18) && n <= ((cx_u64)1 << 31);
    if (j->alone_shared) {
        const mf_shape_t sh = mf_shape(pr, n);
        const cx_u64 size = n > sh.size ? pow2_at_least((cx_u64)sh.size + ALONE_WINDOW) : sh.size;
        const cx_u64 need = size * 2 * sizeof(cx_u32) + ((cx_u64)sizeof(cx_u32) << sh.hash_bits) + WINDOWS * 12 * ALONE_WINDOW;
        if (need > phys_memory() / 2) j->alone_shared = 0;
    }
    j->per_slice = !j->alone && !pr->bt && workers > 1 && j->blocks < workers;
    const int sliced = !j->alone;
    j->first = (cx_u64*)cx_alloc((j->blocks + 1) * sizeof(cx_u64));
    cx_u64 total = 0;
    cx_u64* per = (cx_u64*)cx_alloc((j->blocks ? j->blocks : 1) * sizeof(cx_u64));
    for (cx_u64 b = 0; b < j->blocks; b++) {
        cx_u64 len = b + 1 < j->blocks ? block : n - b * block;
        cx_u64 k = 1;
        if (sliced) {
            k = len / slice + (len % slice != 0);
            if (k < len / slice_max + 1) k = len / slice_max + 1;
            if (k == 0) k = 1;
        }
        per[b] = k;
        j->first[b] = total;
        total += k;
    }
    j->first[j->blocks] = total;
    j->slices = total;
    j->s = (slice_t*)cx_alloc((total ? total : 1) * sizeof(slice_t));
    for (cx_u64 b = 0; b < j->blocks; b++) {
        const cx_u8* data = in + b * block;
        cx_u64 len = b + 1 < j->blocks ? block : n - b * block, k = per[b];
        for (cx_u64 i = 0; i < k; i++) {
            slice_t* s = &j->s[j->first[b] + i];
            memset(s, 0, sizeof *s);
            s->data = data;
            s->len = (cx_usize)len;
            s->start = (cx_usize)(len * i / k);
            s->end = (cx_usize)(len * (i + 1) / k);
            s->win = s->start > reach ? s->start - (cx_usize)reach : 0;
            s->first = i == 0;
        }
    }
    cx_free(per, (j->blocks ? j->blocks : 1) * sizeof(cx_u64));
    /* As many at once as the workers and a quarter of the memory allow. */
    const cx_u64 units = j->per_slice ? total : j->blocks;
    cx_u64 groups = units < workers ? units : workers;
    if (groups > 1) {
        cx_u64 one = sizeof(enc_t) + CHUNK_CODE, most = 0;
        for (cx_u64 k = 0; k < units; k++) {
            const slice_t* s = &j->s[j->per_slice ? k : j->first[k]];
            cx_u64 m = mf_bytes(pr, mf_shape(pr, s->len));
            if (m > most) most = m;
        }
        cx_u64 fit = phys_memory() / 4 / (one + most);
        if (fit < 1) fit = 1;
        if (groups > fit) groups = fit;
    }
    j->groups = groups ? groups : 1;
    if (j->shared) {
        for (cx_u64 b = 0; b < j->blocks; b++) block_shared(pr, j->s + j->first[b], j->first[b + 1] - j->first[b]);
    } else if (j->alone_shared) {
        alone_shared(pr, &j->s[0]);
    } else if (total) {
        cx_parallel(j->groups, job_body, j);
    }
    for (cx_u64 b = 0; b < j->blocks; b++) {
        cx_u64 size = j->alone ? 0 : 1;
        for (cx_u64 k = j->first[b]; k < j->first[b + 1]; k++) size += j->s[k].out.len;
        sizes[b] = size;
    }
    return (cx_u64)(uintptr_t)j;
}

/* How many blocks a job of n bytes in blocks of `block` has. */
cx_u64 cx_xze_blocks(cx_u64 n, cx_u64 block) {
    if (block == 0) return n ? 1 : 0;
    return n / block + (n % block != 0);
}

void cx_xze_take(cx_u64 h, cx_u64 b, cx_u8* dst) {
    job_t* j = (job_t*)(uintptr_t)h;
    for (cx_u64 k = j->first[b]; k < j->first[b + 1]; k++) {
        memcpy(dst, j->s[k].out.p, j->s[k].out.len);
        dst += j->s[k].out.len;
    }
    if (!j->alone) *dst = 0;
}

void cx_xze_end(cx_u64 h) {
    job_t* j = (job_t*)(uintptr_t)h;
    for (cx_u64 k = 0; k < j->slices; k++) {
        if (j->s[k].out.p) cx_free(j->s[k].out.p, j->s[k].out.cap);
    }
    cx_free(j->s, (j->slices ? j->slices : 1) * sizeof(slice_t));
    cx_free(j->first, (j->blocks + 1) * sizeof(cx_u64));
    cx_free(j, sizeof(job_t));
}
