/* Zstandard decompression, behind std.zstd (CLX-181): the frames RFC 8878 defines, with their
 * blocks, Huffman-coded literals, FSE-coded sequences, dictionaries and XXH64 checksums. Linked
 * only into programs that call it.
 *
 * Written from RFC 8878 and the xxHash specification, and made to fail where libzstd fails, which
 * Python's compression.zstd is, as black-box tests found it. It has no libzstd code.
 *
 * The output is the window: a frame decodes straight into the program's buffer, so a match copies
 * from what's already there, with no window to wrap around and nothing copied out. std.zstd
 * reserves the room first when the frame gives its size. */
#include "calx_rt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- what goes wrong ------------------------------------------------------------------------- */

#define ZS_DONE 0
#define ZS_SKIP 1
#define ZS_ROOM 2
#define ZS_FORMAT (-1)       /* not a frame: "Unknown frame descriptor" */
#define ZS_UNSUPPORTED (-2)  /* a reserved bit set: "Unsupported frame parameter" */
#define ZS_WINDOW (-3)       /* "Frame requires too much memory for decoding" */
#define ZS_CORRUPT (-4)      /* "Data corruption detected", and the like */
#define ZS_CHECKSUM (-5)     /* "Restored data doesn't match checksum" */
#define ZS_DICT_WRONG (-6)   /* "Dictionary mismatch" */
#define ZS_TRUNCATED (-7)    /* the data ends inside a frame */
#define ZS_DICT_BAD (-8)     /* a dictionary that isn't one */
#define ZS_TOO_LONG (-9)     /* more than the limit */

/* --- sizes ----------------------------------------------------------------------------------- */

#define BLOCK_MAX ((cx_u32)1 << 17)
/* How far past where it stops a fast copy may read and write. */
#define SLACK 64
#define HUF_LOG_MAX 12
#define LL_MAX 35
#define ML_MAX 52
#define OF_MAX 31
#define LL_LOG 9
#define ML_LOG 9
#define OF_LOG 8
#define MAGIC 0xFD2FB528u
#define SKIP_MAGIC 0x184D2A50u
#define DICT_MAGIC 0xEC30A437u
/* Python decodes a frame in one pass, without checking its window, when the frame gives its size
 * and that fits the first piece of output it makes room for. */
#define ONE_PASS 32768u

static inline cx_u64 ld64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return v;
}

static inline cx_u32 ld32(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}

static inline cx_u32 ld24(const cx_u8* p) {
    return (cx_u32)p[0] | ((cx_u32)p[1] << 8) | ((cx_u32)p[2] << 16);
}

static inline cx_u32 ld16(const cx_u8* p) {
    return (cx_u32)p[0] | ((cx_u32)p[1] << 8);
}

/* The highest set bit of v, which isn't 0. */
static inline cx_u32 high_bit(cx_u32 v) {
    return 31 - (cx_u32)__builtin_clz(v);
}

/* --- XXH64 ----------------------------------------------------------------------------------- */

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static inline cx_u64 rotl(cx_u64 x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline cx_u64 xx_round(cx_u64 acc, cx_u64 in) {
    acc += in * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}

static inline cx_u64 xx_merge(cx_u64 acc, cx_u64 v) {
    acc ^= xx_round(0, v);
    return acc * P1 + P4;
}

cx_u64 cx_zstd_xxh64(const cx_u8* p, cx_usize n, cx_u64 seed) {
    const cx_u8* end = p + n;
    cx_u64 h;
    if (n >= 32) {
        cx_u64 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const cx_u8* limit = end - 32;
        do {
            v1 = xx_round(v1, ld64(p));
            v2 = xx_round(v2, ld64(p + 8));
            v3 = xx_round(v3, ld64(p + 16));
            v4 = xx_round(v4, ld64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xx_merge(h, v1);
        h = xx_merge(h, v2);
        h = xx_merge(h, v3);
        h = xx_merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += (cx_u64)n;
    while (end - p >= 8) {
        h ^= xx_round(0, ld64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (cx_u64)ld32(p) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (cx_u64)*p++ * P5;
        h = rotl(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

/* --- reading bits backwards ------------------------------------------------------------------ */

/* Huffman and FSE streams are read from their last byte back, whose highest set bit marks where
 * the stream starts. `bits` holds the 8 bytes from `p`, and the top `used` of them are read. */
typedef struct {
    cx_u64 bits;
    cx_u32 used;
    const cx_u8* p;
    const cx_u8* start;
} br_t;

/* Starts reading src[0..n); 0 if it's empty or its last byte is 0, which no stream ends with. */
static inline int br_init(br_t* b, const cx_u8* src, cx_usize n) {
    if (n == 0 || src[n - 1] == 0) return 0;
    b->start = src;
    if (n >= 8) {
        b->p = src + n - 8;
        b->bits = ld64(b->p);
    } else {
        cx_u64 v = 0;
        for (cx_usize i = 0; i < n; i++) v |= (cx_u64)src[i] << (8 * i);
        b->p = src;
        b->bits = v;
    }
    /* The bytes a short stream lacks count as read, with the marker's zeros and the marker. */
    b->used = (cx_u32)__builtin_clzll(b->bits) + 1;
    return 1;
}

/* The next n bits, 0 to 56 of them, without taking them. */
static inline cx_u64 br_peek(const br_t* b, cx_u32 n) {
    return ((b->bits << (b->used & 63)) >> 1) >> (63 - n);
}

static inline cx_u64 br_read(br_t* b, cx_u32 n) {
    cx_u64 v = br_peek(b, n);
    b->used += n;
    return v;
}

/* Moves back over the bytes read, so at least 56 bits are there unless the start is near. */
static inline void br_reload(br_t* b) {
    cx_usize can = (cx_usize)(b->p - b->start);
    if (can == 0) return;
    cx_usize back = b->used >> 3;
    if (back > can) back = can;
    b->p -= back;
    b->used -= (cx_u32)(back * 8);
    b->bits = ld64(b->p);
}

/* Whether every bit has been read, and no more. */
static inline int br_done(const br_t* b) {
    return b->p == b->start && b->used == 64;
}

/* --- FSE tables ------------------------------------------------------------------------------ */

/* A state of an FSE table: the next state is `next` plus `bits` bits, and its symbol, for
 * sequences, is a value of `base` plus `extra` bits. For Huffman weights, `base` is the weight. */
typedef struct {
    cx_u16 next;
    cx_u8 bits;
    cx_u8 extra;
    cx_u32 base;
} cell_t;

/* Reads an FSE table's description from p[0..n): the probabilities of symbols up to `max` at most,
 * and the accuracy log, at most `log_max`. Gives how many bytes it took, or 0 if it's broken. */
static cx_usize read_counts(const cx_u8* p, cx_usize n, cx_i16* norm, cx_u32* max, cx_u32* log, cx_u32 log_max) {
    /* The bits are read from the low end of each byte, up, past the end as zeros. */
    cx_u8 pad[8];
    cx_usize bitpos = 0;
#define BITS_AT(k)                                                                    \
    ({                                                                                \
        cx_usize at_ = bitpos >> 3;                                                   \
        cx_u64 w_;                                                                    \
        if (at_ + 8 <= n) {                                                           \
            w_ = ld64(p + at_);                                                       \
        } else {                                                                      \
            memset(pad, 0, 8);                                                        \
            if (at_ < n) memcpy(pad, p + at_, n - at_);                               \
            w_ = ld64(pad);                                                           \
        }                                                                             \
        (cx_u32)((w_ >> (bitpos & 7)) & (((cx_u64)1 << (k)) - 1));                    \
    })
    if (n == 0) return 0;
    const cx_u32 al = BITS_AT(4) + 5;
    bitpos += 4;
    if (al > log_max) return 0;
    int remaining = (1 << al) + 1, threshold = 1 << al;
    cx_u32 nbits = al + 1, sym = 0;
    int zero = 0;
    while (remaining > 1 && sym <= *max) {
        if (zero) {
            cx_u32 n0 = sym;
            for (;;) {
                const cx_u32 r = BITS_AT(2);
                bitpos += 2;
                n0 += r;
                if (r != 3) break;
            }
            if (n0 > *max) return 0;
            while (sym < n0) norm[sym++] = 0;
        }
        const int most = (2 * threshold - 1) - remaining;
        const cx_u32 v = BITS_AT(nbits);
        int count;
        if ((int)(v & (cx_u32)(threshold - 1)) < most) {
            count = (int)(v & (cx_u32)(threshold - 1));
            bitpos += nbits - 1;
        } else {
            count = (int)(v & (cx_u32)(2 * threshold - 1));
            if (count >= threshold) count -= most;
            bitpos += nbits;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[sym++] = (cx_i16)count;
        zero = count == 0;
        while (remaining < threshold) {
            nbits--;
            threshold >>= 1;
        }
    }
#undef BITS_AT
    if (remaining != 1 || bitpos > n * 8) return 0;
    *max = sym - 1;
    *log = al;
    return (bitpos + 7) >> 3;
}

/* Builds the table for norm[0..max] at accuracy `log`, each symbol s turned into base[s] and
 * extra[s]. 0 if the symbols don't spread over it, as only broken counts make them. */
static int build_table(cell_t* t, const cx_i16* norm, cx_u32 max, cx_u32 log, const cx_u32* base, const cx_u8* extra) {
    const cx_u32 size = 1u << log, mask = size - 1;
    cx_u32 high = size - 1;
    cx_u16 next[256];
    cx_u8 sym[1 << LL_LOG];
    for (cx_u32 s = 0; s <= max; s++) {
        if (norm[s] == -1) {
            sym[high--] = (cx_u8)s;
            next[s] = 1;
        } else {
            next[s] = (cx_u16)norm[s];
        }
    }
    const cx_u32 step = (size >> 1) + (size >> 3) + 3;
    cx_u32 pos = 0;
    for (cx_u32 s = 0; s <= max; s++) {
        for (int i = 0; i < norm[s]; i++) {
            sym[pos] = (cx_u8)s;
            do pos = (pos + step) & mask;
            while (pos > high);
        }
    }
    if (pos != 0) return 0;
    for (cx_u32 u = 0; u < size; u++) {
        const cx_u32 s = sym[u], ns = next[s]++;
        const cx_u32 nb = log - high_bit(ns);
        t[u].bits = (cx_u8)nb;
        t[u].next = (cx_u16)((ns << nb) - size);
        t[u].extra = extra[s];
        t[u].base = base[s];
    }
    return 1;
}

/* --- the sequences' codes -------------------------------------------------------------------- */

static const cx_u32 LL_BASE[LL_MAX + 1] = {0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   10,   11,   12,   13,   14,    15,    16,    18,
                                            20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const cx_u8 LL_EXTRA[LL_MAX + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const cx_u32 ML_BASE[ML_MAX + 1] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  12,  13,  14,  15,  16,  17,   18,   19,   20,
                                            21, 22, 23, 24, 25, 26, 27, 28, 29,  30,  31,  32,  33,  34,  35,   37,   39,   41,
                                            43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const cx_u8 ML_EXTRA[ML_MAX + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static cx_u32 OF_BASE[OF_MAX + 1];
static cx_u8 OF_EXTRA[OF_MAX + 1];

/* The predefined distributions, which a block takes unless it gives its own. */
static const cx_i16 LL_NORM[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const cx_i16 ML_NORM[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const cx_i16 OF_NORM[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

static cell_t LL_PRE[64], ML_PRE[64], OF_PRE[32];
/* For the Huffman weights' table, whose symbols are the weights. */
static cx_u32 BYTE_ID[256];
static const cx_u8 BYTE_NONE[256];
static int tables_ready;

static void tables_init(void) {
    for (cx_u32 s = 0; s < 256; s++) BYTE_ID[s] = s;
    for (cx_u32 s = 0; s <= OF_MAX; s++) {
        OF_BASE[s] = (cx_u32)1 << s;
        OF_EXTRA[s] = (cx_u8)s;
    }
    (void)build_table(LL_PRE, LL_NORM, 35, 6, LL_BASE, LL_EXTRA);
    (void)build_table(ML_PRE, ML_NORM, 52, 6, ML_BASE, ML_EXTRA);
    (void)build_table(OF_PRE, OF_NORM, 28, 5, OF_BASE, OF_EXTRA);
    __atomic_store_n(&tables_ready, 1, __ATOMIC_RELEASE);
}

/* --- the decoder's state --------------------------------------------------------------------- */

/* A sequence: its literals' length, its match's length, and its offset, as coded until the
 * offsets are worked out, and then the distance back. */
typedef struct {
    cx_u32 ll, ml, off;
} seq_t;

/* Each sequence makes 3 bytes at least, so a block holds no more than this many. */
#define SEQ_MAX (BLOCK_MAX / 3 + 1)

/* The tables and repeated offsets a block leaves for the next, or a dictionary for a frame. */
typedef struct {
    cell_t ll[1 << LL_LOG], of[1 << OF_LOG], ml[1 << ML_LOG];
    /* A Huffman entry: the symbol in the high byte, its length in bits in the low. */
    cx_u16 huf[1 << HUF_LOG_MAX];
    cx_u32 ll_log, of_log, ml_log, huf_log;
    cx_u8 ll_ok, of_ok, ml_ok, huf_ok;
    cx_u32 rep[3];
} ent_t;

typedef struct {
    ent_t e;
    cx_u64 size; /* the frame's content size, or ~0 */
    cx_u64 window;
    cx_u32 block_max;
    cx_u32 check;
    cx_usize at;    /* the next block's header in the input */
    cx_usize start; /* where the frame's output starts */
    void* par;      /* the parallel decoder's state, a par_t, or 0 */
    int par_mode;   /* whether this frame is decoded on all the processors */
    int par_begun;  /* whether blocks_par has started on it */
    cx_u8 lits[BLOCK_MAX + SLACK];
} ctx_t;

cx_usize cx_zstd_ctx_size(void) { return sizeof(ctx_t); }

/* A frame gives its size and has at least this much, or doesn't, to decode on all the processors. */
#define PAR_MIN ((cx_u64)1 << 20)
cx_usize cx_zstd_ent_size(void) { return sizeof(ent_t); }

static void ent_reset(ent_t* e) {
    e->ll_ok = e->of_ok = e->ml_ok = e->huf_ok = 0;
    e->rep[0] = 1;
    e->rep[1] = 4;
    e->rep[2] = 8;
}

/* --- Huffman --------------------------------------------------------------------------------- */

/* Reads a Huffman tree's description from p[0..n) into e's table. Gives the bytes it took, or 0. */
static cx_usize read_huffman(ent_t* e, const cx_u8* p, cx_usize n) {
    cx_u8 w[256];
    cx_u32 count;
    cx_usize used;
    if (n < 1) return 0;
    const cx_u32 head = p[0];
    if (head >= 128) {
        /* The weights four bits each, the first in the high half of a byte. */
        count = head - 127;
        used = 1 + (count + 1) / 2;
        if (used > n) return 0;
        for (cx_u32 i = 0; i < count; i++) w[i] = (i & 1) ? (p[1 + i / 2] & 15) : (p[1 + i / 2] >> 4);
    } else {
        /* The weights FSE-coded in `head` bytes, by two states taking turns. */
        used = 1 + (cx_usize)head;
        if (head == 0 || used > n) return 0;
        /* As libzstd reads them, the weights' table may give any byte a probability. */
        cx_i16 norm[256];
        cx_u32 max = 255, log;
        const cx_usize took = read_counts(p + 1, head, norm, &max, &log, 6);
        if (took == 0 || took >= head) return 0;
        cell_t t[64];
        if (!build_table(t, norm, max, log, BYTE_ID, BYTE_NONE)) return 0;
        br_t b;
        if (!br_init(&b, p + 1 + took, head - took)) return 0;
        cx_u32 s1 = (cx_u32)br_read(&b, log), s2 = (cx_u32)br_read(&b, log);
        count = 0;
        /* Each state gives its symbol, then moves on; once a move would read past the start, the
         * other state gives its symbol and that's the last. */
        for (;;) {
            if (count > 253) return 0;
            w[count++] = (cx_u8)t[s1].base;
            br_reload(&b);
            s1 = t[s1].next + (cx_u32)br_read(&b, t[s1].bits);
            if (b.used > 64) {
                w[count++] = (cx_u8)t[s2].base;
                break;
            }
            if (count > 253) return 0;
            w[count++] = (cx_u8)t[s2].base;
            br_reload(&b);
            s2 = t[s2].next + (cx_u32)br_read(&b, t[s2].bits);
            if (b.used > 64) {
                w[count++] = (cx_u8)t[s1].base;
                break;
            }
        }
    }
    /* The last symbol's weight is what makes the total a power of two. */
    cx_u32 total = 0, ranks[HUF_LOG_MAX + 2] = {0};
    for (cx_u32 i = 0; i < count; i++) {
        if (w[i] > HUF_LOG_MAX) return 0;
        ranks[w[i]]++;
        total += w[i] ? (1u << (w[i] - 1)) : 0;
    }
    if (total == 0) return 0;
    const cx_u32 log = high_bit(total) + 1;
    if (log > HUF_LOG_MAX) return 0;
    const cx_u32 rest = (1u << log) - total;
    if (rest & (rest - 1)) return 0;
    const cx_u32 last = high_bit(rest) + 1;
    if (count >= 256) return 0;
    w[count++] = (cx_u8)last;
    ranks[last]++;
    if (ranks[1] < 2 || (ranks[1] & 1)) return 0;
    /* The longest codes first: each weight's symbols in order, 2^(weight-1) entries each. */
    cx_u32 pos = 0;
    for (cx_u32 wt = 1; wt <= log; wt++) {
        if (!ranks[wt]) continue;
        const cx_u32 len = 1u << (wt - 1);
        for (cx_u32 s = 0; s < count; s++) {
            if (w[s] != wt) continue;
            const cx_u16 entry = (cx_u16)((s << 8) | (log + 1 - wt));
            for (cx_u32 i = 0; i < len; i++) e->huf[pos + i] = entry;
            pos += len;
        }
    }
    e->huf_log = log;
    e->huf_ok = 1;
    return used;
}

/* Decodes one Huffman stream, src[0..n), with table t of `log` bits, into exactly `count` bytes at o. */
static int huf_one(const cx_u16* t, cx_u32 log, const cx_u8* src, cx_usize n, cx_u8* o, cx_usize count) {
    br_t b;
    if (!br_init(&b, src, n)) return 0;
    const cx_u32 sh = 64 - log;
    cx_u8* end = o + count;
    /* Four at a time while the stream has room to move back, as four codes are 48 bits at most. */
    while (end - o >= 4 && b.p - b.start >= 8) {
        br_reload(&b);
        cx_u16 x = t[(b.bits << b.used) >> sh];
        o[0] = (cx_u8)(x >> 8);
        b.used += x & 0xFF;
        x = t[(b.bits << b.used) >> sh];
        o[1] = (cx_u8)(x >> 8);
        b.used += x & 0xFF;
        x = t[(b.bits << b.used) >> sh];
        o[2] = (cx_u8)(x >> 8);
        b.used += x & 0xFF;
        x = t[(b.bits << b.used) >> sh];
        o[3] = (cx_u8)(x >> 8);
        b.used += x & 0xFF;
        o += 4;
    }
    while (o < end) {
        br_reload(&b);
        if (b.used > 64) return 0;
        const cx_u16 x = t[br_peek(&b, log)];
        *o++ = (cx_u8)(x >> 8);
        b.used += x & 0xFF;
    }
    br_reload(&b);
    return br_done(&b);
}

/* Decodes four Huffman streams at once, s[k][0..n[k]) into o[k][0..count[k]), the last count the
 * smallest. They don't depend on each other, so the processor works on all four together. */
static int huf_four(const cx_u16* t, cx_u32 log, const cx_u8* const* s, const cx_usize* n, cx_u8* const* o, const cx_usize* count) {
    br_t b[4];
    for (int k = 0; k < 4; k++) {
        if (!br_init(&b[k], s[k], n[k])) return 0;
    }
    const cx_u32 sh = 64 - log;
    cx_u8 *o0 = o[0], *o1 = o[1], *o2 = o[2], *o3 = o[3];
    cx_u64 w0 = b[0].bits, w1 = b[1].bits, w2 = b[2].bits, w3 = b[3].bits;
    cx_u32 u0 = b[0].used, u1 = b[1].used, u2 = b[2].used, u3 = b[3].used;
    const cx_u8 *p0 = b[0].p, *p1 = b[1].p, *p2 = b[2].p, *p3 = b[3].p;
    cx_u8* const end3 = o[3] + count[3];
    /* While every stream can move back a whole reload, and the shortest has four more to give. */
    while (end3 - o3 >= 4 && p0 - b[0].start >= 8 && p1 - b[1].start >= 8 && p2 - b[2].start >= 8 && p3 - b[3].start >= 8) {
        p0 -= u0 >> 3;
        u0 &= 7;
        w0 = ld64(p0);
        p1 -= u1 >> 3;
        u1 &= 7;
        w1 = ld64(p1);
        p2 -= u2 >> 3;
        u2 &= 7;
        w2 = ld64(p2);
        p3 -= u3 >> 3;
        u3 &= 7;
        w3 = ld64(p3);
#define HUF_ONE(w, u, op, i)                     \
    do {                                         \
        const cx_u16 x_ = t[((w) << (u)) >> sh]; \
        (op)[i] = (cx_u8)(x_ >> 8);              \
        (u) += x_ & 0xFF;                        \
    } while (0)
        HUF_ONE(w0, u0, o0, 0);
        HUF_ONE(w1, u1, o1, 0);
        HUF_ONE(w2, u2, o2, 0);
        HUF_ONE(w3, u3, o3, 0);
        HUF_ONE(w0, u0, o0, 1);
        HUF_ONE(w1, u1, o1, 1);
        HUF_ONE(w2, u2, o2, 1);
        HUF_ONE(w3, u3, o3, 1);
        HUF_ONE(w0, u0, o0, 2);
        HUF_ONE(w1, u1, o1, 2);
        HUF_ONE(w2, u2, o2, 2);
        HUF_ONE(w3, u3, o3, 2);
        HUF_ONE(w0, u0, o0, 3);
        HUF_ONE(w1, u1, o1, 3);
        HUF_ONE(w2, u2, o2, 3);
        HUF_ONE(w3, u3, o3, 3);
#undef HUF_ONE
        o0 += 4;
        o1 += 4;
        o2 += 4;
        o3 += 4;
    }
    /* The rest of each, one at a time. */
    cx_u8* at[4] = {o0, o1, o2, o3};
    b[0].bits = w0, b[0].used = u0, b[0].p = p0;
    b[1].bits = w1, b[1].used = u1, b[1].p = p1;
    b[2].bits = w2, b[2].used = u2, b[2].p = p2;
    b[3].bits = w3, b[3].used = u3, b[3].p = p3;
    for (int k = 0; k < 4; k++) {
        br_t* r = &b[k];
        cx_u8* op = at[k];
        cx_u8* const end = o[k] + count[k];
        while (op < end) {
            br_reload(r);
            if (r->used > 64) return 0;
            const cx_u16 x = t[br_peek(r, log)];
            *op++ = (cx_u8)(x >> 8);
            r->used += x & 0xFF;
        }
        br_reload(r);
        if (!br_done(r)) return 0;
    }
    return 1;
}

/* --- literals -------------------------------------------------------------------------------- */

/* A block's literals, as their section's header gives them. */
typedef struct {
    cx_u8 kind; /* 0 raw, 1 one byte repeated, 2 Huffman-coded */
    cx_u8 tree; /* Huffman-coded with a tree of their own, which lit_head read */
    cx_u8 four; /* in four streams */
    cx_usize n;
    const cx_u8* src; /* raw: the literals; repeated: the byte; Huffman: the streams */
    cx_usize src_n;   /* Huffman: the streams' bytes, with their jump table */
    cx_usize took;    /* the section's bytes */
} lit_t;

/* Reads the literals section's header at p[0..n), and a Huffman tree into e's table. 0 if it's
 * broken. */
static int lit_head(ent_t* e, cx_u32 block_max, const cx_u8* p, cx_usize n, lit_t* L) {
    if (n < 2) return 0;
    const cx_u32 type = p[0] & 3, format = (p[0] >> 2) & 3;
    L->tree = 0;
    L->four = 0;
    if (type < 2) {
        cx_usize head, size;
        if (format == 0 || format == 2) {
            head = 1;
            size = p[0] >> 3;
        } else if (format == 1) {
            head = 2;
            size = ld16(p) >> 4;
        } else {
            if (n < 3) return 0;
            head = 3;
            size = ld24(p) >> 4;
        }
        if (size > block_max) return 0;
        L->kind = (cx_u8)type;
        L->n = size;
        L->src = p + head;
        if (type == 0) {
            if (head + size > n) return 0;
            L->took = head + size;
        } else {
            if (head + 1 > n) return 0;
            L->took = head + 1;
        }
        return 1;
    }
    if (n < 5) return 0;
    cx_usize head, size, csize;
    const int single = format == 0;
    const cx_u32 h = ld32(p);
    if (format < 2) {
        head = 3;
        size = (h >> 4) & 0x3FF;
        csize = (h >> 14) & 0x3FF;
    } else if (format == 2) {
        head = 4;
        size = (h >> 4) & 0x3FFF;
        csize = h >> 18;
    } else {
        head = 5;
        size = (h >> 4) & 0x3FFFF;
        csize = (h >> 22) + ((cx_usize)p[4] << 10);
    }
    if (size > block_max) return 0;
    if (!single && size < 6) return 0;
    if (head + csize > n) return 0;
    const cx_u8* q = p + head;
    cx_usize qn = csize;
    if (type == 2) {
        const cx_usize took = read_huffman(e, q, qn);
        if (took == 0) return 0;
        q += took;
        qn -= took;
        L->tree = 1;
    } else if (!e->huf_ok) {
        return 0;
    }
    if (!single) {
        if (qn < 10) return 0;
        if ((cx_usize)ld16(q) + ld16(q + 2) + ld16(q + 4) + 6 > qn) return 0;
        L->four = 1;
    }
    L->kind = 2;
    L->n = size;
    L->src = q;
    L->src_n = qn;
    L->took = head + csize;
    return 1;
}

/* Puts the literals where they can be read, with room after them for a fast copy: raw ones where
 * they are when they can be, and the rest decoded into buf, with Huffman table t of `log` bits.
 * 0 if they're broken. */
static int lit_decode(const cx_u16* t, cx_u32 log, const lit_t* L, const cx_u8* in_end, cx_u8* buf, const cx_u8** lits) {
    if (L->kind == 0) {
        if ((cx_usize)(in_end - (L->src + L->n)) >= SLACK) {
            *lits = L->src;
        } else {
            memcpy(buf, L->src, L->n);
            *lits = buf;
        }
        return 1;
    }
    *lits = buf;
    if (L->kind == 1) {
        memset(buf, L->src[0], L->n);
        return 1;
    }
    if (!L->four) return huf_one(t, log, L->src, L->src_n, buf, L->n);
    const cx_u8* q = L->src;
    const cx_usize l1 = ld16(q), l2 = ld16(q + 2), l3 = ld16(q + 4);
    const cx_usize l4 = L->src_n - 6 - l1 - l2 - l3, seg = (L->n + 3) / 4;
    const cx_u8* s = q + 6;
    const cx_u8* const streams[4] = {s, s + l1, s + l1 + l2, s + l1 + l2 + l3};
    const cx_usize sizes[4] = {l1, l2, l3, l4};
    cx_u8* const outs[4] = {buf, buf + seg, buf + 2 * seg, buf + 3 * seg};
    const cx_usize counts[4] = {seg, seg, seg, L->n - 3 * seg};
    return huf_four(t, log, streams, sizes, outs, counts);
}

/* Whether the literals need room of their own. */
static int lit_needs_room(const lit_t* L, const cx_u8* in_end) {
    return L->kind != 0 || (cx_usize)(in_end - (L->src + L->n)) < SLACK;
}

/* --- sequences ------------------------------------------------------------------------------- */

/* Sets a table up by its mode: the predefined one, one symbol, one described, or the last one. */
static cx_usize read_table(cell_t* t, cx_u32* log, cx_u8* ok, cx_u32 mode, const cx_u8* p, cx_usize n, cx_u32 max, cx_u32 log_max, const cell_t* pre, cx_u32 pre_log,
                           const cx_u32* base, const cx_u8* extra, int* bad) {
    if (mode == 0) {
        memcpy(t, pre, sizeof(cell_t) << pre_log);
        *log = pre_log;
        *ok = 1;
        return 0;
    }
    if (mode == 1) {
        if (n < 1 || p[0] > max) {
            *bad = 1;
            return 0;
        }
        t[0].next = 0;
        t[0].bits = 0;
        t[0].extra = extra[p[0]];
        t[0].base = base[p[0]];
        *log = 0;
        *ok = 1;
        return 1;
    }
    if (mode == 2) {
        cx_i16 norm[64];
        cx_u32 m = max, l;
        const cx_usize took = read_counts(p, n, norm, &m, &l, log_max);
        if (took == 0 || !build_table(t, norm, m, l, base, extra)) {
            *bad = 1;
            return 0;
        }
        *log = l;
        *ok = 1;
        return took;
    }
    if (!*ok) *bad = 1;
    return 0;
}

/* Reads the sequences section's head from p[0..n): how many there are, their modes, and their
 * tables into e. Gives where the bitstream starts, or 0 if it's broken; with none, the section is
 * that byte. */
static cx_usize seq_head(ent_t* e, const cx_u8* p, cx_usize n, cx_u32* count, cx_u32* modes_out) {
    if (n < 1) return 0;
    cx_u32 nseq = p[0];
    cx_usize at = 1;
    *modes_out = 0xFC;
    if (nseq == 0) {
        *count = 0;
        return n == 1 ? 1 : 0;
    }
    if (nseq >= 128) {
        if (nseq < 255) {
            if (n < 2) return 0;
            nseq = ((nseq - 128) << 8) + p[1];
            at = 2;
        } else {
            if (n < 3) return 0;
            nseq = p[1] + ((cx_u32)p[2] << 8) + 0x7F00;
            at = 3;
        }
    }
    if (at >= n) return 0;
    const cx_u32 modes = p[at++];
    if (modes & 3) return 0;
    int bad = 0;
    at += read_table(e->ll, &e->ll_log, &e->ll_ok, modes >> 6, p + at, n - at, LL_MAX, LL_LOG, LL_PRE, 6, LL_BASE, LL_EXTRA, &bad);
    if (bad) return 0;
    at += read_table(e->of, &e->of_log, &e->of_ok, (modes >> 4) & 3, p + at, n - at, OF_MAX, OF_LOG, OF_PRE, 5, OF_BASE, OF_EXTRA, &bad);
    if (bad) return 0;
    at += read_table(e->ml, &e->ml_log, &e->ml_ok, (modes >> 2) & 3, p + at, n - at, ML_MAX, ML_LOG, ML_PRE, 6, ML_BASE, ML_EXTRA, &bad);
    if (bad || at >= n) return 0;
    *count = nseq;
    *modes_out = modes;
    return at;
}

/* The three tables a block's sequences are decoded with. */
typedef struct {
    const cell_t *ll, *of, *ml;
    cx_u32 ll_log, of_log, ml_log;
} tabs_t;

/* Copies n bytes and up to 15 more, 16 at a time, from s on to d, where s is behind d by 16 or
 * more or not in the way. */
static inline void copy16(cx_u8* d, const cx_u8* s, cx_usize n) {
    cx_u8* e = d + n;
    do {
        memcpy(d, s, 16);
        d += 16;
        s += 16;
    } while (d < e);
}

/* As copy16, 32 at a time and up to 31 more, where s is behind d by 32 or more. Most literals and
 * matches are shorter than 32, so most take the one copy. */
static inline void copy32(cx_u8* d, const cx_u8* s, cx_usize n) {
    memcpy(d, s, 32);
    if (n <= 32) return;
    cx_u8* e = d + n;
    d += 32;
    s += 32;
    do {
        memcpy(d, s, 32);
        d += 32;
        s += 32;
    } while (d < e);
}

/* The frame's output so far, and the dictionary's content before it, which matches reach into. */
typedef struct {
    cx_u8* base;  /* where the frame starts */
    cx_u8* bound; /* where this block must stop */
    cx_u8* fast;  /* where a fast copy may still start: SLACK - 32 past the bound */
    const cx_u8* dict;
    cx_usize dict_n;
} out_t;

/* Copies a match of `ml` bytes from `off` back to op. 0 if it reaches past what there is. */
static inline int copy_match(const out_t* o, cx_u8* op, cx_u32 off, cx_u32 ml) {
    const cx_usize have = (cx_usize)(op - o->base);
    if (off > have) {
        /* Into the dictionary, then on from the frame's start. */
        const cx_usize back = off - have;
        if (back > o->dict_n) return 0;
        const cx_u8* s = o->dict + o->dict_n - back;
        if (back >= ml) {
            memcpy(op, s, ml);
            return 1;
        }
        memcpy(op, s, back);
        op += back;
        ml -= (cx_u32)back;
    }
    const cx_u8* s = op - off;
    if (op + ml <= o->fast) {
        if (off >= 32) {
            copy32(op, s, ml);
            return 1;
        }
        if (off >= 16) {
            copy16(op, s, ml);
            return 1;
        }
        /* A repeating pattern shorter than 16 bytes. Its first 8 bytes are made so that the copy
         * can go on from a whole number of patterns back, 8 or more: under 8, four bytes one by
         * one and four more from where the pattern lines up again. libzstd's decoder does the
         * same; the tables are worked out here for each offset. */
        if (off < 8) {
            static const cx_u8 next[8] = {0, 1, 2, 1, 4, 4, 4, 4};
            static const cx_u8 back[8] = {0, 8, 8, 9, 8, 10, 12, 14};
            op[0] = s[0];
            op[1] = s[1];
            op[2] = s[2];
            op[3] = s[3];
            memcpy(op + 4, s + next[off], 4);
            s = op + 8 - back[off];
        } else {
            memcpy(op, s, 8);
            s += 8;
        }
        cx_u8* d = op + 8;
        cx_u8* const e = op + ml;
        while (d < e) {
            memcpy(d, s, 8);
            d += 8;
            s += 8;
        }
        return 1;
    }
    for (cx_u32 i = 0; i < ml; i++) op[i] = s[i];
    return 1;
}

/* Works a coded offset out from the last three, which it moves on: 0 if it comes to 0. */
static inline cx_u32 offset_of(cx_u32 v, cx_u32 ll, cx_u32* r0, cx_u32* r1, cx_u32* r2) {
    if (v > 3) {
        const cx_u32 off = v - 3;
        *r2 = *r1;
        *r1 = *r0;
        *r0 = off;
        return off;
    }
    const cx_u32 k = v - 1 + (ll == 0);
    if (k == 0) return *r0;
    const cx_u32 off = k == 3 ? *r0 - 1 : k == 1 ? *r1 : *r2;
    if (off == 0) return 0;
    if (k != 1) *r2 = *r1;
    *r1 = *r0;
    *r0 = off;
    return off;
}

/* Decodes `count` sequences from the bitstream p[0..n) and carries each out as it's decoded, with
 * the literals, onto the output at *opp: the way one thread goes fastest. 0 if they're broken. */
static int seq_run(ent_t* e, const cx_u8* p, cx_usize n, cx_u32 count, const cx_u8* lit, cx_usize lit_n, const out_t* o, cx_u8** opp) {
    cx_u8* op = *opp;
    const cx_u8* lit_end = lit + lit_n;
    if (count) {
        br_t b;
        if (!br_init(&b, p, n)) return 0;
        cx_u32 ls = (cx_u32)br_read(&b, e->ll_log);
        cx_u32 os = (cx_u32)br_read(&b, e->of_log);
        cx_u32 ms = (cx_u32)br_read(&b, e->ml_log);
        br_reload(&b);
        cx_u32 r0 = e->rep[0], r1 = e->rep[1], r2 = e->rep[2];
        for (cx_u32 i = 0; i < count; i++) {
            const cell_t lc = e->ll[ls], oc = e->of[os], mc = e->ml[ms];
            cx_u32 v, ml, ll;
            /* A reload leaves at least 57 bits. Extra bits of 30 or fewer and the states' 26 at
             * most fit in them, so most sequences take one reload, at their end, as in libzstd's
             * decoder. */
            if (__builtin_expect(oc.extra + mc.extra + lc.extra <= 30, 1)) {
                v = oc.base + (cx_u32)br_read(&b, oc.extra);
                ml = mc.base + (cx_u32)br_read(&b, mc.extra);
                ll = lc.base + (cx_u32)br_read(&b, lc.extra);
            } else {
                v = oc.base + (cx_u32)br_read(&b, oc.extra);
                br_reload(&b);
                ml = mc.base + (cx_u32)br_read(&b, mc.extra);
                ll = lc.base + (cx_u32)br_read(&b, lc.extra);
                br_reload(&b);
            }
            if (i + 1 < count) {
                ls = lc.next + (cx_u32)br_read(&b, lc.bits);
                ms = mc.next + (cx_u32)br_read(&b, mc.bits);
                os = oc.next + (cx_u32)br_read(&b, oc.bits);
            }
            br_reload(&b);
            const cx_u32 off = offset_of(v, ll, &r0, &r1, &r2);
            if (off == 0) return 0;
            /* The literals, then the match. */
            if ((cx_usize)(lit_end - lit) < ll) return 0;
            if ((cx_usize)(o->bound - op) < (cx_usize)ll + ml) return 0;
            if (op + ll <= o->fast) {
                copy32(op, lit, ll);
            } else {
                memcpy(op, lit, ll);
            }
            op += ll;
            lit += ll;
            if (!copy_match(o, op, off, ml)) return 0;
            op += ml;
        }
        if (!br_done(&b)) return 0;
        e->rep[0] = r0;
        e->rep[1] = r1;
        e->rep[2] = r2;
    }
    const cx_usize rest = (cx_usize)(lit_end - lit);
    if ((cx_usize)(o->bound - op) < rest) return 0;
    memcpy(op, lit, rest);
    *opp = op + rest;
    return 1;
}

/* Decodes `count` sequences from the bitstream p[0..n) into s, with tables T. With `lit_n`
 * literals, they must use no more than there are, and make no more than `most` bytes: *made is
 * how many they do. 0 if they're broken. */
static int seq_decode(const tabs_t* T, const cx_u8* p, cx_usize n, cx_u32 count, seq_t* s, cx_usize lit_n, cx_usize most, cx_usize* made) {
    br_t b;
    if (!br_init(&b, p, n)) return 0;
    const cell_t* const llt = T->ll;
    const cell_t* const oft = T->of;
    const cell_t* const mlt = T->ml;
    cx_u32 ls = (cx_u32)br_read(&b, T->ll_log);
    cx_u32 os = (cx_u32)br_read(&b, T->of_log);
    cx_u32 ms = (cx_u32)br_read(&b, T->ml_log);
    br_reload(&b);
    cx_usize lits = 0, matches = 0;
    for (cx_u32 i = 0; i < count; i++) {
        const cell_t lc = llt[ls], oc = oft[os], mc = mlt[ms];
        cx_u32 v, ml, ll;
        /* One reload for most sequences, as in seq_run. */
        if (__builtin_expect(oc.extra + mc.extra + lc.extra <= 30, 1)) {
            v = oc.base + (cx_u32)br_read(&b, oc.extra);
            ml = mc.base + (cx_u32)br_read(&b, mc.extra);
            ll = lc.base + (cx_u32)br_read(&b, lc.extra);
        } else {
            v = oc.base + (cx_u32)br_read(&b, oc.extra);
            br_reload(&b);
            ml = mc.base + (cx_u32)br_read(&b, mc.extra);
            ll = lc.base + (cx_u32)br_read(&b, lc.extra);
            br_reload(&b);
        }
        if (i + 1 < count) {
            ls = lc.next + (cx_u32)br_read(&b, lc.bits);
            ms = mc.next + (cx_u32)br_read(&b, mc.bits);
            os = oc.next + (cx_u32)br_read(&b, oc.bits);
        }
        br_reload(&b);
        s[i].ll = ll;
        s[i].ml = ml;
        s[i].off = v;
        lits += ll;
        matches += ml;
    }
    if (!br_done(&b) || lits > lit_n || matches > most || lit_n > most - matches) return 0;
    *made = lit_n + matches;
    return 1;
}

/* How many sequences ahead a match's bytes are fetched into the cache. */
#define AHEAD 8

/* Carries out sequences that seq_decode decoded and checked, working their offsets out from
 * `rep` as it goes, with the literals, onto the output at *opp. 0 if an offset comes to 0 or a
 * match reaches past what there is. */
static int seq_exec(seq_t* s, cx_u32 count, cx_u32* rep, const cx_u8* lit, cx_usize lit_n, const out_t* o, cx_u8** opp) {
    cx_u8* op = *opp;
    const cx_u8* const lit_end = lit + lit_n;
    cx_u32 r0 = rep[0], r1 = rep[1], r2 = rep[2];
    /* The offsets are worked out AHEAD sequences early, so their matches can be fetched. */
    const cx_u32 first = count < AHEAD ? count : AHEAD;
    cx_u8* ahead = op;
    for (cx_u32 j = 0; j < first; j++) {
        const cx_u32 off = offset_of(s[j].off, s[j].ll, &r0, &r1, &r2);
        if (off == 0) return 0;
        s[j].off = off;
        __builtin_prefetch(ahead + s[j].ll - off);
        ahead += s[j].ll + s[j].ml;
    }
    for (cx_u32 i = 0; i < count; i++) {
        if (i + AHEAD < count) {
            seq_t* f = &s[i + AHEAD];
            const cx_u32 off = offset_of(f->off, f->ll, &r0, &r1, &r2);
            if (off == 0) return 0;
            f->off = off;
            __builtin_prefetch(ahead + f->ll - off);
            ahead += f->ll + f->ml;
        }
        const cx_u32 ll = s[i].ll, ml = s[i].ml, off = s[i].off;
        if (op + ll <= o->fast) {
            copy32(op, lit, ll);
        } else {
            memcpy(op, lit, ll);
        }
        op += ll;
        lit += ll;
        if (!copy_match(o, op, off, ml)) return 0;
        op += ml;
    }
    const cx_usize rest = (cx_usize)(lit_end - lit);
    memcpy(op, lit, rest);
    *opp = op + rest;
    rep[0] = r0;
    rep[1] = r1;
    rep[2] = r2;
    return 1;
}

/* --- dictionaries ---------------------------------------------------------------------------- */

/* Reads a dictionary into `ent`: info[0] its ID, info[1] where its content starts. One that doesn't
 * start with the dictionary magic number is all content. */
cx_i32 cx_zstd_dict(const cx_u8* d, cx_usize n, cx_u8* ent_bytes, cx_u64* info) {
    if (!__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE)) tables_init();
    ent_t* e = (ent_t*)ent_bytes;
    ent_reset(e);
    info[0] = 0;
    info[1] = 0;
    if (n < 8 || ld32(d) != DICT_MAGIC) return ZS_DONE;
    info[0] = ld32(d + 4);
    cx_usize at = 8;
    /* The literals' Huffman table, then the offsets', match lengths' and literal lengths' FSE
     * tables, then the three repeated offsets. */
    const cx_usize took = read_huffman(e, d + at, n - at);
    if (took == 0) return ZS_DICT_BAD;
    at += took;
    int bad = 0;
    at += read_table(e->of, &e->of_log, &e->of_ok, 2, d + at, n - at, OF_MAX, OF_LOG, OF_PRE, 5, OF_BASE, OF_EXTRA, &bad);
    if (bad) return ZS_DICT_BAD;
    at += read_table(e->ml, &e->ml_log, &e->ml_ok, 2, d + at, n - at, ML_MAX, ML_LOG, ML_PRE, 6, ML_BASE, ML_EXTRA, &bad);
    if (bad) return ZS_DICT_BAD;
    at += read_table(e->ll, &e->ll_log, &e->ll_ok, 2, d + at, n - at, LL_MAX, LL_LOG, LL_PRE, 6, LL_BASE, LL_EXTRA, &bad);
    if (bad || n - at < 12) return ZS_DICT_BAD;
    const cx_usize content = n - at - 12;
    for (int i = 0; i < 3; i++) {
        const cx_u32 r = ld32(d + at + 4 * i);
        if (r == 0 || r > content) return ZS_DICT_BAD;
        e->rep[i] = r;
    }
    info[1] = at + 12;
    return ZS_DONE;
}

/* --- frames ---------------------------------------------------------------------------------- */

/* Whether the frame whose blocks start at `at` is all there, with its checksum: what Python's
 * decoder looks for before it decodes a frame in one pass. */
static int frame_whole(const cx_u8* in, cx_usize n, cx_usize at, int check) {
    for (;;) {
        if (n - at < 3) return 0;
        const cx_u32 h = ld24(in + at);
        const cx_u32 type = (h >> 1) & 3, size = h >> 3;
        if (type == 3) return 0;
        const cx_usize on_wire = type == 1 ? 1 : size;
        if (n - at - 3 < on_wire) return 0;
        at += 3 + on_wire;
        if (h & 1) break;
    }
    return !check || n - at >= 4;
}

/* Starts the frame at in[at..n): info[0] its content size or ~0, info[1] its window, info[2] where
 * its blocks start, or for a skippable frame where it ends. Gives ZS_DONE, ZS_SKIP, or an error.
 * `ent` is a dictionary's tables or 0, `dict_id` its ID. */
cx_i32 cx_zstd_frame(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_usize at, const cx_u8* ent, cx_u32 dict_id, cx_u64 window_max, cx_u64* info) {
    if (!__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE)) tables_init();
    ctx_t* c = (ctx_t*)ctx;
    const cx_usize left = n - at;
    const cx_u8* p = in + at;
    if (left < 4) {
        /* What there is must start one of the magic numbers. */
        cx_u8 m[4], s[4];
        const cx_u32 mv = MAGIC, sv = SKIP_MAGIC;
        memcpy(m, &mv, 4);
        memcpy(s, &sv, 4);
        int zstd = 1, skip = 1;
        for (cx_usize i = 0; i < left; i++) {
            if (p[i] != m[i]) zstd = 0;
            if (i < 3 ? p[i] != s[i] : (p[i] & 0xF0) != (s[i] & 0xF0)) skip = 0;
        }
        return zstd || skip ? ZS_TRUNCATED : ZS_FORMAT;
    }
    const cx_u32 magic = ld32(p);
    if ((magic & 0xFFFFFFF0u) == SKIP_MAGIC) {
        if (left < 8) return ZS_TRUNCATED;
        const cx_u64 size = ld32(p + 4);
        if (left - 8 < size) return ZS_TRUNCATED;
        info[2] = at + 8 + size;
        return ZS_SKIP;
    }
    if (magic != MAGIC) return ZS_FORMAT;
    if (left < 5) return ZS_TRUNCATED;
    const cx_u32 desc = p[4];
    const cx_u32 fcs_code = desc >> 6, single = (desc >> 5) & 1, check = (desc >> 2) & 1, did_code = desc & 3;
    static const cx_u8 DID_BYTES[4] = {0, 1, 2, 4};
    static const cx_u8 FCS_BYTES[4] = {0, 2, 4, 8};
    const cx_usize fcs_bytes = fcs_code == 0 ? single : FCS_BYTES[fcs_code];
    const cx_usize head = 5 + !single + DID_BYTES[did_code] + fcs_bytes;
    if (left < head) return ZS_TRUNCATED;
    if (desc & 0x08) return ZS_UNSUPPORTED;
    cx_usize q = 5;
    cx_u64 window = 0;
    if (!single) {
        const cx_u32 wd = p[q++];
        const cx_u32 wlog = 10 + (wd >> 3);
        if (wlog > 31) return ZS_WINDOW;
        const cx_u64 wbase = (cx_u64)1 << wlog;
        window = wbase + (wbase >> 3) * (wd & 7);
    }
    cx_u32 did = 0;
    if (did_code == 1) did = p[q];
    else if (did_code == 2) did = ld16(p + q);
    else if (did_code == 3) did = ld32(p + q);
    q += DID_BYTES[did_code];
    cx_u64 size = ~(cx_u64)0;
    if (fcs_bytes == 1) size = p[q];
    else if (fcs_bytes == 2) size = ld16(p + q) + 256;
    else if (fcs_bytes == 4) size = ld32(p + q);
    else if (fcs_bytes == 8) size = ld64(p + q);
    q += fcs_bytes;
    if (single) window = size;
    if (did != 0 && did != dict_id) return ZS_DICT_WRONG;
    if (window > window_max && !(size != ~(cx_u64)0 && size <= ONE_PASS && frame_whole(in, n, at + q, (int)check))) return ZS_WINDOW;
    /* A dictionary's tables, or none but its repeated offsets. */
    const ent_t* d = (const ent_t*)ent;
    if (d && (d->huf_ok | d->ll_ok | d->of_ok | d->ml_ok)) {
        memcpy(&c->e, d, sizeof(ent_t));
    } else {
        ent_reset(&c->e);
        if (d) memcpy(c->e.rep, d->rep, sizeof c->e.rep);
    }
    c->size = size;
    c->window = window;
    c->block_max = window < BLOCK_MAX ? (cx_u32)window : BLOCK_MAX;
    c->check = check;
    c->at = at + q;
    c->par_mode = cx_workers() > 1 && (size == ~(cx_u64)0 || size >= PAR_MIN);
    c->par_begun = 0;
    info[0] = size;
    info[1] = window;
    info[2] = at + q;
    return ZS_DONE;
}

/* --- frames on all the processors ------------------------------------------------------------ */

/* A frame's blocks depend on each other in two ways. Their tables carry over, which only reading
 * their headers in order needs; and their matches reach into what blocks before them made, which
 * only carrying their sequences out in order needs. Decoding their literals and sequences, which
 * takes most of the time, needs neither, given the tables. So one thread reads a run of blocks'
 * headers and builds their tables, every thread decodes the run's blocks, and one thread carries
 * them out in order while the next run is decoded, and another works the checksum out over what
 * the run before made. */

/* XXH64 of bytes given a piece at a time. */
typedef struct {
    cx_u64 v1, v2, v3, v4, total;
    cx_u8 mem[32];
    cx_u32 mem_n;
} xxh_t;

static void xxh_start(xxh_t* s) {
    s->v1 = P1 + P2;
    s->v2 = P2;
    s->v3 = 0;
    s->v4 = (cx_u64)0 - P1;
    s->total = 0;
    s->mem_n = 0;
}

static void xxh_add(xxh_t* s, const cx_u8* p, cx_usize n) {
    s->total += n;
    if (s->mem_n + n < 32) {
        memcpy(s->mem + s->mem_n, p, n);
        s->mem_n += (cx_u32)n;
        return;
    }
    if (s->mem_n) {
        const cx_usize fill = 32 - s->mem_n;
        memcpy(s->mem + s->mem_n, p, fill);
        s->v1 = xx_round(s->v1, ld64(s->mem));
        s->v2 = xx_round(s->v2, ld64(s->mem + 8));
        s->v3 = xx_round(s->v3, ld64(s->mem + 16));
        s->v4 = xx_round(s->v4, ld64(s->mem + 24));
        p += fill;
        n -= fill;
        s->mem_n = 0;
    }
    cx_u64 v1 = s->v1, v2 = s->v2, v3 = s->v3, v4 = s->v4;
    while (n >= 32) {
        v1 = xx_round(v1, ld64(p));
        v2 = xx_round(v2, ld64(p + 8));
        v3 = xx_round(v3, ld64(p + 16));
        v4 = xx_round(v4, ld64(p + 24));
        p += 32;
        n -= 32;
    }
    s->v1 = v1, s->v2 = v2, s->v3 = v3, s->v4 = v4;
    memcpy(s->mem, p, n);
    s->mem_n = (cx_u32)n;
}

static cx_u64 xxh_end(const xxh_t* s) {
    cx_u64 h;
    if (s->total >= 32) {
        h = rotl(s->v1, 1) + rotl(s->v2, 7) + rotl(s->v3, 12) + rotl(s->v4, 18);
        h = xx_merge(h, s->v1);
        h = xx_merge(h, s->v2);
        h = xx_merge(h, s->v3);
        h = xx_merge(h, s->v4);
    } else {
        h = s->v3 + P5;
    }
    h += s->total;
    const cx_u8* p = s->mem;
    const cx_u8* end = p + s->mem_n;
    while (end - p >= 8) {
        h ^= xx_round(0, ld64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (cx_u64)ld32(p) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (cx_u64)*p++ * P5;
        h = rotl(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

/* Room taken in pieces of at least a MiB, and given back all at once. */
typedef struct piece {
    struct piece* next;
    cx_usize cap, used;
} piece_t;

typedef struct {
    piece_t* first;
    piece_t* cur;
} pool_t;

static void* pool_take(pool_t* a, cx_usize n) {
    n = (n + 63) & ~(cx_usize)63;
    for (;;) {
        piece_t* c = a->cur;
        if (c && c->cap - c->used >= n) {
            void* p = (cx_u8*)c + 64 + c->used;
            c->used += n;
            return p;
        }
        if (c && c->next) {
            a->cur = c->next;
            continue;
        }
        const cx_usize cap = n > ((cx_usize)1 << 20) ? n : ((cx_usize)1 << 20);
        piece_t* fresh = (piece_t*)cx_alloc(64 + cap);
        fresh->next = 0;
        fresh->cap = cap;
        fresh->used = 0;
        if (c) {
            c->next = fresh;
        } else {
            a->first = fresh;
        }
        a->cur = fresh;
    }
}

static void pool_reset(pool_t* a) {
    for (piece_t* c = a->first; c; c = c->next) c->used = 0;
    a->cur = a->first;
}

static void pool_free(pool_t* a) {
    piece_t* c = a->first;
    while (c) {
        piece_t* next = c->next;
        cx_free(c, 64 + c->cap);
        c = next;
    }
    a->first = a->cur = 0;
}

/* A copy of the Huffman table, or of the three sequence tables, as a block decodes with them. */
typedef struct {
    cx_u32 log;
    cx_u16 t[1 << HUF_LOG_MAX];
} huf_copy_t;

typedef struct {
    cx_u32 ll_log, of_log, ml_log;
    cell_t ll[1 << LL_LOG], of[1 << OF_LOG], ml[1 << ML_LOG];
} fse_copy_t;

static void copy_huf(huf_copy_t* d, const ent_t* e) {
    d->log = e->huf_log;
    memcpy(d->t, e->huf, sizeof(cx_u16) << e->huf_log);
}

static void copy_fse(fse_copy_t* d, const ent_t* e) {
    d->ll_log = e->ll_log;
    d->of_log = e->of_log;
    d->ml_log = e->ml_log;
    if (e->ll_ok) memcpy(d->ll, e->ll, sizeof(cell_t) << e->ll_log);
    if (e->of_ok) memcpy(d->of, e->of, sizeof(cell_t) << e->of_log);
    if (e->ml_ok) memcpy(d->ml, e->ml, sizeof(cell_t) << e->ml_log);
}

/* A block of a run: what its header said, and once it's decoded, what it makes. */
typedef struct {
    cx_u8 type; /* 0 raw, 1 RLE, 2 compressed */
    cx_i32 err; /* what decoding it found: 0, or a ZS_ code */
    cx_u32 size;
    const cx_u8* src;
    lit_t lit;
    const huf_copy_t* huf;
    const fse_copy_t* fse;
    const cx_u8* bits;
    cx_usize bits_n;
    cx_u32 count;
    cx_u8* lit_buf;
    const cx_u8* lits;
    seq_t* seqs;
    cx_usize made;
} blk_t;

/* A run of blocks, and what came after them: an error, or the frame's end. */
typedef struct {
    blk_t* b;
    cx_u32 n, cap;
    cx_i32 err;
    pool_t pool;
    /* The tables the run started with, which its first blocks may repeat. */
    huf_copy_t first_huf;
    fse_copy_t first_fse;
} run_t;

/* Three runs take turns: one is carried out while the next is decoded and the one after that has
 * its headers read. */
typedef struct {
    run_t runs[3];
    int carry; /* the run decoded and waiting to be carried out, or -1 */
    int dec;   /* the run whose headers are read, waiting to be decoded, or -1 */
    int ended; /* the headers have been read to the frame's end, or to an error */
    cx_u32 per_run;
    const huf_copy_t* huf; /* the tables the next block repeats */
    const fse_copy_t* fse;
    xxh_t hash;
    cx_usize hashed; /* how much of the frame's output the hash has */
} par_t;

/* Reads the next block's header and tables at c->at into k. Gives 0, 1 for the frame's last
 * block, or an error. */
static cx_i32 pre_block(ctx_t* c, par_t* P, run_t* R, const cx_u8* in, cx_usize n, blk_t* k) {
    const cx_usize at = c->at;
    if (n - at < 3) return ZS_TRUNCATED;
    const cx_u32 h = ld24(in + at);
    const cx_u32 last = h & 1, type = (h >> 1) & 3, size = h >> 3;
    if (type == 3) return ZS_CORRUPT;
    /* The bytes a block takes are checked before they're waited for; what an RLE block makes,
     * after its byte. */
    const cx_usize on_wire = type == 1 ? 1 : size;
    if (on_wire > c->block_max) return ZS_CORRUPT;
    if (n - at - 3 < on_wire) return ZS_TRUNCATED;
    if (size > c->block_max) return ZS_CORRUPT;
    const cx_u8* src = in + at + 3;
    k->type = (cx_u8)type;
    k->err = 0;
    k->size = size;
    k->src = src;
    if (type == 2) {
        if (!lit_head(&c->e, c->block_max, src, size, &k->lit)) return ZS_CORRUPT;
        if (k->lit.tree) {
            huf_copy_t* t = (huf_copy_t*)pool_take(&R->pool, sizeof(huf_copy_t));
            copy_huf(t, &c->e);
            P->huf = t;
        }
        k->huf = P->huf;
        cx_u32 count, modes;
        const cx_usize head = seq_head(&c->e, src + k->lit.took, size - k->lit.took, &count, &modes);
        if (head == 0 || count > SEQ_MAX) return ZS_CORRUPT;
        if (count && (modes & 0xFC) != 0xFC) {
            fse_copy_t* t = (fse_copy_t*)pool_take(&R->pool, sizeof(fse_copy_t));
            copy_fse(t, &c->e);
            P->fse = t;
        }
        k->fse = P->fse;
        k->count = count;
        k->bits = src + k->lit.took + head;
        k->bits_n = size - k->lit.took - head;
        const cx_u8* in_end = in + n;
        k->lit_buf = lit_needs_room(&k->lit, in_end) ? (cx_u8*)pool_take(&R->pool, k->lit.n + SLACK) : 0;
        k->seqs = count ? (seq_t*)pool_take(&R->pool, count * sizeof(seq_t)) : 0;
    } else {
        k->made = size;
    }
    c->at = at + 3 + on_wire;
    return last ? 1 : 0;
}

/* Reads the next run's headers. */
static void pre_run(ctx_t* c, par_t* P, run_t* R, const cx_u8* in, cx_usize n) {
    /* The tables blocks repeat may be in the pool about to be reused, so they're kept aside. */
    if (c->e.huf_ok) {
        copy_huf(&R->first_huf, &c->e);
        P->huf = &R->first_huf;
    }
    copy_fse(&R->first_fse, &c->e);
    P->fse = &R->first_fse;
    pool_reset(&R->pool);
    R->n = 0;
    R->err = 0;
    while (R->n < P->per_run && !P->ended) {
        const cx_i32 r = pre_block(c, P, R, in, n, &R->b[R->n]);
        if (r < 0) {
            R->err = r;
            P->ended = 1;
            break;
        }
        R->n++;
        if (r == 1) P->ended = 1;
    }
}

/* Decodes a compressed block's literals and sequences. */
static void decode_block(blk_t* k, const cx_u8* in_end, cx_u32 block_max) {
    const cx_u8* lits;
    if (!lit_decode(k->huf ? k->huf->t : 0, k->huf ? k->huf->log : 0, &k->lit, in_end, k->lit_buf, &lits)) {
        k->err = ZS_CORRUPT;
        return;
    }
    k->lits = lits;
    if (k->count) {
        const fse_copy_t* f = k->fse;
        const tabs_t T = {f->ll, f->of, f->ml, f->ll_log, f->of_log, f->ml_log};
        if (!seq_decode(&T, k->bits, k->bits_n, k->count, k->seqs, k->lit.n, block_max, &k->made)) {
            k->err = ZS_CORRUPT;
            return;
        }
    } else {
        if (k->lit.n > block_max) {
            k->err = ZS_CORRUPT;
            return;
        }
        k->made = k->lit.n;
    }
}

/* Carries a decoded run out, in order, onto out at *pos. */
static cx_i32 carry_run(ctx_t* c, run_t* R, cx_u8* out, cx_usize start, cx_usize* pos_io, cx_usize limit, const cx_u8* dict, cx_usize dict_n) {
    cx_usize pos = *pos_io;
    for (cx_u32 i = 0; i < R->n; i++) {
        blk_t* k = &R->b[i];
        if (k->err) return k->err;
        cx_usize most = c->block_max;
        if (c->size != ~(cx_u64)0) {
            const cx_u64 made = pos - start;
            const cx_u64 left = c->size > made ? c->size - made : 0;
            if (left < most) most = (cx_usize)left;
        }
        if (k->made > most) return ZS_CORRUPT;
        if (k->type == 0) {
            memcpy(out + pos, k->src, k->size);
        } else if (k->type == 1) {
            memset(out + pos, k->src[0], k->size);
        } else {
            out_t o;
            o.base = out + start;
            o.bound = out + pos + k->made;
            o.fast = o.bound + SLACK - 32;
            o.dict = dict;
            o.dict_n = dict_n;
            cx_u8* op = out + pos;
            if (!seq_exec(k->seqs, k->count, c->e.rep, k->lits, k->lit.n, &o, &op)) return ZS_CORRUPT;
        }
        pos += k->made;
        *pos_io = pos;
        if (pos > limit) return ZS_TOO_LONG;
    }
    return R->err;
}

/* One step: carry a run out, read the headers of the run after next, work the checksum on over
 * what's been carried out, and decode the next run. The first three are a task each. */
typedef struct {
    ctx_t* c;
    par_t* P;
    run_t* carry; /* or 0 */
    run_t* read;  /* or 0 */
    run_t* dec;   /* or 0 */
    cx_u32* jobs; /* dec's compressed blocks */
    cx_u32 njobs;
    int hash; /* whether there's hashing to do */
    const cx_u8* hash_p;
    cx_usize hash_n;
    const cx_u8* in;
    cx_usize n;
    cx_u8* out;
    cx_usize start, pos, limit, dict_n;
    const cx_u8* dict;
    cx_i32 result;
} step_t;

static void step_body(void* v, cx_u64 begin, cx_u64 end) {
    step_t* st = (step_t*)v;
    const int has[3] = {st->carry != 0, st->read != 0, st->hash};
    for (cx_u64 i = begin; i < end; i++) {
        /* The i-th of the tasks there are. */
        cx_u64 k = i;
        int which = 0;
        for (; which < 3; which++) {
            if (!has[which]) continue;
            if (k == 0) break;
            k--;
        }
        if (which == 0) {
            st->result = carry_run(st->c, st->carry, st->out, st->start, &st->pos, st->limit, st->dict, st->dict_n);
        } else if (which == 1) {
            pre_run(st->c, st->P, st->read, st->in, st->n);
        } else if (which == 2) {
            xxh_add(&st->P->hash, st->hash_p, st->hash_n);
        } else {
            decode_block(&st->dec->b[st->jobs[k]], st->in + st->n, st->c->block_max);
        }
    }
}

/* What a decoded run makes, up to its first broken block. */
static cx_usize run_made(const run_t* R) {
    cx_usize made = 0;
    for (cx_u32 i = 0; i < R->n; i++) {
        if (R->b[i].err) break;
        made += R->b[i].made;
    }
    return made;
}

static void run_room(run_t* R, cx_u32 cap) {
    if (R->cap >= cap) return;
    if (R->b) cx_free(R->b, R->cap * sizeof(blk_t));
    R->b = (blk_t*)cx_alloc(cap * sizeof(blk_t));
    R->cap = cap;
}

static void par_free(par_t* P) {
    for (int i = 0; i < 3; i++) {
        pool_free(&P->runs[i].pool);
        if (P->runs[i].b) cx_free(P->runs[i].b, P->runs[i].cap * sizeof(blk_t));
    }
    cx_free(P, sizeof(par_t));
}

/* As cx_zstd_blocks, on all the processors. */
static cx_i32 blocks_par(ctx_t* c, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize start, cx_usize* len, cx_usize cap, cx_usize limit, const cx_u8* dict, cx_usize dict_n,
                         cx_u64* info) {
    par_t* P = (par_t*)c->par;
    if (!c->par_begun) {
        if (!P) {
            P = (par_t*)cx_alloc(sizeof(par_t));
            memset(P, 0, sizeof *P);
            c->par = P;
        }
        P->per_run = (cx_u32)cx_workers() * 2;
        if (P->per_run > 256) P->per_run = 256;
        for (int i = 0; i < 3; i++) run_room(&P->runs[i], P->per_run);
        P->carry = -1;
        P->dec = -1;
        P->ended = 0;
        P->huf = 0;
        P->fse = 0;
        xxh_start(&P->hash);
        P->hashed = start;
        c->par_begun = 1;
    }
    cx_usize pos = *len;
    const int known = c->size != ~(cx_u64)0;
    cx_u32 jobs[256];
    for (;;) {
        run_t* X = P->carry >= 0 ? &P->runs[P->carry] : 0;
        run_t* Y = P->dec >= 0 ? &P->runs[P->dec] : 0;
        /* The run to carry out needs room for what it makes, unless the frame's size gave it. */
        if (X && !known) {
            const cx_usize need = run_made(X) + SLACK;
            if (cap - pos < need) {
                *len = pos;
                info[1] = need;
                return ZS_ROOM;
            }
        }
        run_t* Z = 0;
        if (!P->ended) {
            int free = 0;
            while (free == P->carry || free == P->dec) free++;
            Z = &P->runs[free];
        }
        if (!X && !Y) {
            if (!Z) break;
            /* Nothing to read the first run's headers alongside. */
            pre_run(c, P, Z, in, n);
            P->dec = (int)(Z - P->runs);
            continue;
        }
        step_t st;
        memset(&st, 0, sizeof st);
        st.c = c;
        st.P = P;
        st.carry = X;
        st.read = Z;
        st.dec = Y;
        st.in = in;
        st.n = n;
        st.out = out;
        st.start = start;
        st.pos = pos;
        st.limit = limit;
        st.dict = dict;
        st.dict_n = dict_n;
        st.jobs = jobs;
        if (Y) {
            for (cx_u32 i = 0; i < Y->n; i++) {
                if (Y->b[i].type == 2) jobs[st.njobs++] = i;
            }
        }
        if (c->check && pos > P->hashed) {
            st.hash = 1;
            st.hash_p = out + P->hashed;
            st.hash_n = pos - P->hashed;
        }
        const cx_u64 tasks = (X ? 1 : 0) + (Z ? 1 : 0) + (st.hash ? 1 : 0) + st.njobs;
        if (tasks) cx_parallel(tasks, step_body, &st);
        if (st.hash) P->hashed = pos;
        pos = st.pos;
        if (X && st.result != 0) {
            *len = pos;
            return st.result;
        }
        P->carry = Y ? (int)(Y - P->runs) : -1;
        P->dec = Z && (Z->n || Z->err) ? (int)(Z - P->runs) : -1;
    }
    c->par_begun = 0;
    if (known && (cx_u64)(pos - start) != c->size) return ZS_CORRUPT;
    cx_usize at = c->at;
    if (c->check) {
        if (n - at < 4) return ZS_TRUNCATED;
        xxh_add(&P->hash, out + P->hashed, pos - P->hashed);
        const cx_u32 want = ld32(in + at);
        if (want != (cx_u32)xxh_end(&P->hash)) return ZS_CHECKSUM;
        at += 4;
    }
    *len = pos;
    c->at = at;
    info[0] = at;
    return ZS_DONE;
}

/* Decodes the frame's blocks onto out, from *len, as far as `cap` has room for, and fails with
 * ZS_TOO_LONG past `limit`, a length of out. Gives ZS_DONE at the frame's end, with info[0] where
 * it ends in the input, or ZS_ROOM when out needs more room for the next block, or an error.
 * `start` is where the frame's output starts in out, and dict[0..dict_n) the dictionary's content,
 * which comes before it. */
cx_i32 cx_zstd_blocks(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize start, cx_usize* len, cx_usize cap, cx_usize limit, const cx_u8* dict, cx_usize dict_n,
                      cx_u64* info) {
    ctx_t* c = (ctx_t*)ctx;
    if (c->par_mode) return blocks_par(c, in, n, out, start, len, cap, limit, dict, dict_n, info);
    const cx_u8* in_end = in + n;
    cx_usize at = c->at;
    cx_usize pos = *len;
    for (;;) {
        if (n - at < 3) return ZS_TRUNCATED;
        const cx_u32 h = ld24(in + at);
        const cx_u32 last = h & 1, type = (h >> 1) & 3, size = h >> 3;
        if (type == 3) return ZS_CORRUPT;
        /* The bytes a block takes are checked before they're waited for; what an RLE block
         * makes, after its byte. */
        const cx_usize on_wire = type == 1 ? 1 : size;
        if (on_wire > c->block_max) return ZS_CORRUPT;
        if (n - at - 3 < on_wire) return ZS_TRUNCATED;
        if (size > c->block_max) return ZS_CORRUPT;
        /* How much this block may make: the block's limit, and what the frame's size leaves. */
        cx_usize most = c->block_max;
        if (c->size != ~(cx_u64)0) {
            const cx_u64 made = pos - start;
            const cx_u64 left = c->size > made ? c->size - made : 0;
            if (left < most) most = (cx_usize)left;
        }
        if (cap - pos < most + SLACK) {
            c->at = at;
            *len = pos;
            info[1] = most + SLACK;
            return ZS_ROOM;
        }
        const cx_u8* src = in + at + 3;
        if (type == 0) {
            if (size > most) return ZS_CORRUPT;
            memcpy(out + pos, src, size);
            pos += size;
        } else if (type == 1) {
            if (size > most) return ZS_CORRUPT;
            memset(out + pos, src[0], size);
            pos += size;
        } else {
            lit_t L;
            if (!lit_head(&c->e, c->block_max, src, size, &L)) return ZS_CORRUPT;
            const cx_u8* lits;
            if (!lit_decode(c->e.huf, c->e.huf_log, &L, in_end, c->lits, &lits)) return ZS_CORRUPT;
            const cx_usize took = L.took;
            out_t o;
            o.base = out + start;
            o.bound = out + pos + most;
            o.fast = o.bound + SLACK - 32;
            o.dict = dict;
            o.dict_n = dict_n;
            cx_u8* op = out + pos;
            cx_u32 count, modes;
            const cx_usize head = seq_head(&c->e, src + took, size - took, &count, &modes);
            if (head == 0) return ZS_CORRUPT;
            if (!seq_run(&c->e, src + took + head, size - took - head, count, lits, L.n, &o, &op)) return ZS_CORRUPT;
            pos = (cx_usize)(op - out);
        }
        at += 3 + on_wire;
        if (pos > limit) return ZS_TOO_LONG;
        if (last) break;
    }
    if (c->size != ~(cx_u64)0 && (cx_u64)(pos - start) != c->size) return ZS_CORRUPT;
    if (c->check) {
        if (n - at < 4) return ZS_TRUNCATED;
        const cx_u32 want = ld32(in + at);
        const cx_u32 got = (cx_u32)cx_zstd_xxh64(out + start, pos - start, 0);
        if (want != got) return ZS_CHECKSUM;
        at += 4;
    }
    *len = pos;
    c->at = at;
    info[0] = at;
    return ZS_DONE;
}

/* Sets a new context up, before its first frame. */
void cx_zstd_init(cx_u8* ctx) {
    ctx_t* c = (ctx_t*)ctx;
    c->par = 0;
    c->par_mode = 0;
    c->par_begun = 0;
}

/* Gives back what a context took, after its last frame. */
void cx_zstd_end(cx_u8* ctx) {
    ctx_t* c = (ctx_t*)ctx;
    if (c->par) par_free((par_t*)c->par);
    c->par = 0;
}
