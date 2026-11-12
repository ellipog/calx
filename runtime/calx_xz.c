/* LZMA and LZMA2 decoding, the xz filters, and CRC-64, behind std.xz (CLX-173): what the .xz
 * format's blocks and the legacy .lzma format hold. std.xz reads the containers; this decodes what
 * they hold. Linked only into programs that call it.
 *
 * The decoders are written from the .xz file format spec 1.2.1, the LZMA SDK's description of
 * LZMA, and liblzma's behaviour, which Python's lzma module is, as black-box tests found it. The
 * branch filters are defined by the code that made them, so they follow liblzma's simple filters,
 * by Igor Pavlov and Lasse Collin, which are in the public domain (0BSD from xz 5.6).
 *
 * The output is the dictionary: a block's text is decoded straight into the program's buffer, so a
 * match copies from what's already there, with no window to wrap around and nothing copied out.
 * std.xz reserves the room first, from the LZMA2 chunks' sizes, so decoding seldom stops midway.
 * Nothing is written past where the text ends, so blocks decode side by side in one buffer. */
#include "calx_rt.h"

#include <string.h>

/* --- the decoder's state --------------------------------------------------------------------- */

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
#define MATCH_MAX 273

typedef cx_u16 prob;

typedef struct {
    prob choice, choice2;
    prob low[POS_STATES_MAX][8];
    prob mid[POS_STATES_MAX][8];
    prob high[256];
} len_probs;

typedef struct {
    /* The probabilities, which a reset sets to a half. */
    prob is_match[STATES][POS_STATES_MAX];
    prob is_rep[STATES], is_rep_g0[STATES], is_rep_g1[STATES], is_rep_g2[STATES];
    prob is_rep0_long[STATES][POS_STATES_MAX];
    prob dist_slot[DIST_STATES][64];
    prob dist_special[FULL_DISTANCES - END_POS_MODEL];
    prob align[1 << ALIGN_BITS];
    len_probs match_len, rep_len;
    prob literal[0x300 << 4]; /* lc + lp is at most 4 */
    /* The state machine and the last four distances, less one. */
    cx_u32 state, rep0, rep1, rep2, rep3;
    cx_u32 lc, lp, pb;
    /* The range decoder, between calls. */
    cx_u32 range, code;
    /* Where it is in the input and the output, and where the text since the dictionary was last
     * reset starts in the output. */
    cx_usize at, pos, start;
    /* How far back a match may reach. */
    cx_u64 dict;
    /* For .lzma: where the text ends if the header said, or all ones. */
    cx_u64 end;
    cx_u32 phase;
    /* Whether more input may come, for std.xz's Decompressor: LZMA stops while a symbol's most
     * input is left, and LZMA2 at a chunk that isn't all there, so that each can carry on. */
    cx_u32 partial;
} dec_t;

_Static_assert(sizeof(dec_t) <= CX_XZ_CTX, "CX_XZ_CTX is too small");

/* Sets the probabilities to a half and the state to the start. */
static void lzma_reset(dec_t* d) {
    prob* p = &d->is_match[0][0];
    prob* e = d->literal + (0x300u << (d->lc + d->lp));
    while (p < e) *p++ = ONE / 2;
    d->state = 0;
    d->rep0 = d->rep1 = d->rep2 = d->rep3 = 0;
}

/* Reads lc, lp and pb from a properties byte: 0, or -1 if it's past 224 or lc + lp is past 4,
 * which liblzma refuses too. */
static int lzma_props(dec_t* d, cx_u32 b) {
    if (b > (4 * 5 + 4) * 9 + 8) return -1;
    d->pb = b / 45;
    b -= d->pb * 45;
    d->lp = b / 9;
    d->lc = b - d->lp * 9;
    return d->lc + d->lp > 4 ? -1 : 0;
}

/* How far back matches may reach with a dictionary of `size` bytes: liblzma's dictionary is at
 * least 4 KiB, rounded up to 16 bytes. */
static cx_u64 dict_reach(cx_u64 size) {
    if (size < 4096) size = 4096;
    return (size + 15) & ~(cx_u64)15;
}

/* Starts the range decoder on the 5 bytes at in[d->at]: a 0, then the code. 0, -1 if the first
 * byte isn't 0, which liblzma sees before the rest come, or -2 if there aren't 5. */
static int rc_init(dec_t* d, const cx_u8* in, cx_usize n) {
    const cx_u8* p = in + d->at;
    if (n > d->at && p[0] != 0) return -1;
    if (n - d->at < 5) return -2;
    d->code = (cx_u32)p[1] << 24 | (cx_u32)p[2] << 16 | (cx_u32)p[3] << 8 | p[4];
    d->range = 0xFFFFFFFFu;
    d->at += 5;
    return 0;
}

/* --- LZMA ------------------------------------------------------------------------------------ */

/* The range decoder's steps work on the locals `range`, `code`, `in` and `end`. With FAST, at least
 * FAST_INPUT bytes of input are left, more than a symbol can take, so they don't look; without it,
 * they jump to `short_input` when the input runs out. */
#define FAST_INPUT 64

#define NORM(FAST)                                        \
    if (range < TOP) {                                    \
        if (!(FAST) && in == end) goto short_input;       \
        range <<= 8;                                      \
        code = (code << 8) | *in++;                       \
    }

/* A bit with the probability at `p`, for a decision: runs ZERO or ONE_. */
#define BIT(FAST, p, ZERO, ONE_)                          \
    do {                                                  \
        NORM(FAST)                                        \
        prob* p_ = (p);                                   \
        cx_u32 bound_ = (range >> BITS) * *p_;            \
        if (code < bound_) {                              \
            range = bound_;                               \
            *p_ += (prob)((ONE - *p_) >> MOVE);           \
            ZERO;                                         \
        } else {                                          \
            range -= bound_;                              \
            code -= bound_;                               \
            *p_ -= (prob)(*p_ >> MOVE);                   \
            ONE_;                                         \
        }                                                 \
    } while (0)

/* A bit with the probability `pr`, which is stored back updated at `slot`, worked out without a
 * branch, as the bits of literals and distances are as good as random to a branch predictor. A 0
 * moves the probability up by (ONE - p) >> MOVE and a 1 down by p >> MOVE, which is p less
 * (p - 2017 or p) >> MOVE in the low 16 bits, which are all that's stored. */
#define TDEC(FAST, slot, pr, bit)                                                 \
    do {                                                                          \
        NORM(FAST)                                                                \
        cx_u32 bound_ = (range >> BITS) * (pr), rest_ = range - bound_, less_;    \
        /* The borrow of code - bound is the bit's opposite, and every choice below \
         * takes it, as liblzma's x86-64 steps take one subtraction's carry. */    \
        cx_u32 zero_ = __builtin_sub_overflow(code, bound_, &less_);             \
        (bit) = zero_ ^ 1;                                                        \
        range = zero_ ? bound_ : rest_;                                           \
        code = zero_ ? code : less_;                                              \
        *(slot) = (prob)((pr) - ((zero_ ? (pr) - (ONE - 31) : (pr)) >> MOVE));    \
    } while (0)

/* A step down a bit tree at `probs`: decodes the bit at node `sym`, whose probability is `pr`,
 * into `bit` and onto the end of `sym`. It loads both children's probabilities
 * before it knows which it goes to, so the next step doesn't wait for a load. LAST is the bottom
 * step, which loads nothing. */
#define TSTEP(FAST, probs, sym, pr, bit, LAST)                                    \
    do {                                                                          \
        cx_u32 c0_ = 0, c1_ = 0;                                                  \
        if (!(LAST)) {                                                            \
            c0_ = (probs)[2 * (sym)];                                             \
            c1_ = (probs)[2 * (sym) + 1];                                         \
        }                                                                         \
        TDEC(FAST, &(probs)[sym], pr, bit);                                       \
        (sym) = 2 * (sym) + (bit);                                                \
        if (!(LAST)) (pr) = (bit) ? c1_ : c0_;                                    \
    } while (0)

/* A bit tree of `n` bits from `probs`, most significant first, into `out`. */
#define TREE(FAST, probs, n, out)                                                  \
    do {                                                                           \
        cx_u32 sym_ = 1, pr_ = (probs)[1], b_;                                     \
        for (int k_ = 0; k_ < (n) - 1; k_++) TSTEP(FAST, probs, sym_, pr_, b_, 0); \
        TSTEP(FAST, probs, sym_, pr_, b_, 1);                                      \
        (out) = sym_ - (1u << (n));                                                \
    } while (0)

/* A bit tree's bits, least significant first, onto `rep0` from bit 0. */
#define RTREE(FAST, probs, n)                                                     \
    do {                                                                          \
        cx_u32 sym_ = 1, pr_ = (probs)[1], b_;                                    \
        for (cx_u32 i_ = 0; i_ + 1 < (n); i_++) {                                 \
            TSTEP(FAST, probs, sym_, pr_, b_, 0);                                 \
            rep0 |= b_ << i_;                                                     \
        }                                                                         \
        TSTEP(FAST, probs, sym_, pr_, b_, 1);                                     \
        rep0 |= b_ << ((n) - 1);                                                  \
    } while (0)

/* A length's symbol, 0 to 271, from the length probabilities at `lp`. */
#define LENGTH(FAST, lp, pos_state, out)                                                          \
    do {                                                                                          \
        cx_u32 v_;                                                                                \
        BIT(FAST, &(lp)->choice, TREE(FAST, (lp)->low[pos_state], 3, v_); (out) = v_,             \
            BIT(FAST, &(lp)->choice2, TREE(FAST, (lp)->mid[pos_state], 3, v_); (out) = v_ + 8,    \
                TREE(FAST, (lp)->high, 8, v_); (out) = v_ + 16));                                 \
    } while (0)

/* A match's distance, less one, into `rep0`, for the length symbol `len`. All ones is the end
 * marker. */
#define DISTANCE(FAST, len)                                                                       \
    do {                                                                                          \
        cx_u32 ds_ = (len) < DIST_STATES ? (len) : DIST_STATES - 1, slot_;                        \
        TREE(FAST, d->dist_slot[ds_], 6, slot_);                                                  \
        if (slot_ < 4) {                                                                          \
            rep0 = slot_;                                                                         \
        } else {                                                                                  \
            cx_u32 direct_ = (slot_ >> 1) - 1;                                                    \
            rep0 = (2 | (slot_ & 1)) << direct_;                                                  \
            if (slot_ < END_POS_MODEL) {                                                          \
                /* This slot's tree, which starts at 1: the probabilities from one before. */     \
                prob* tree_ = d->dist_special + rep0 - slot_ - 1;                                 \
                RTREE(FAST, tree_, direct_);                                                      \
            } else {                                                                              \
                /* The middle bits are even odds, a halving of the range each, onto the end of    \
                 * the top two. */                                                                \
                cx_u32 v_ = 2 | (slot_ & 1);                                                      \
                for (cx_u32 i_ = direct_ - ALIGN_BITS; i_ > 0; i_--) {                            \
                    NORM(FAST)                                                                    \
                    range >>= 1;                                                                  \
                    code -= range;                                                                \
                    cx_u32 t_ = 0u - (code >> 31); /* all ones when code was below: a 0 */        \
                    v_ = (v_ << 1) + (t_ + 1);                                                    \
                    code += range & t_;                                                           \
                }                                                                                 \
                rep0 = v_ << ALIGN_BITS;                                                          \
                RTREE(FAST, d->align, ALIGN_BITS);                                                \
            }                                                                                     \
        }                                                                                         \
    } while (0)

/* One symbol: a literal, a match, or a repeat of one of the last four distances. A literal goes
 * on with `continue`; the end marker jumps to `marker`. Positions count from where the dictionary
 * was last reset, at `base`, and `prev` is the byte before `upos`. */
#define SYMBOL(FAST)                                                                              \
    {                                                                                             \
        const cx_usize pos_state = upos & pb_mask;                                                \
        int literal_ = 0;                                                                         \
        BIT(FAST, &d->is_match[state][pos_state], literal_ = 1, literal_ = 0);                    \
        if (literal_) {                                                                           \
            prob* probs = d->literal + 3 * ((((upos << 8) + prev) & lit_mask) << lc);             \
            cx_u32 sym = 1, b;                                                                    \
            if (state < 7) {                                                                      \
                cx_u32 pr = probs[1];                                                             \
                for (int k = 0; k < 7; k++) TSTEP(FAST, probs, sym, pr, b, 0);                    \
                TSTEP(FAST, probs, sym, pr, b, 1);                                                \
                state = state < 4 ? 0 : state - 3;                                                \
            } else {                                                                              \
                /* After a match, the byte the last distance points at guides the bits until    \
                 * one differs from it. */                                                        \
                cx_u32 match = (cx_u32)base[upos - rep0 - 1] << 1, offs = 0x100;                  \
                cx_u32 idx = offs + (match & offs) + 1, pr = probs[idx];                          \
                for (int k = 0; k < 7; k++) {                                                     \
                    /* Where each bit would go next, loaded before it's known. */                 \
                    cx_u32 mb = match & offs, m1 = match << 1;                                    \
                    cx_u32 o0 = offs & ~mb, o1 = offs & mb;                                       \
                    cx_u32 i0 = o0 + (m1 & o0) + 2 * sym, i1 = o1 + (m1 & o1) + 2 * sym + 1;      \
                    cx_u32 c0 = probs[i0], c1 = probs[i1];                                        \
                    TDEC(FAST, &probs[idx], pr, b);                                               \
                    sym = 2 * sym + b;                                                            \
                    offs = b ? o1 : o0;                                                           \
                    idx = b ? i1 : i0;                                                            \
                    pr = b ? c1 : c0;                                                             \
                    match = m1;                                                                   \
                }                                                                                 \
                TDEC(FAST, &probs[idx], pr, b);                                                   \
                sym = 2 * sym + b;                                                                \
                state = state < 10 ? state - 3 : state - 6;                                       \
            }                                                                                     \
            prev = sym & 0xFF;                                                                    \
            base[upos++] = (cx_u8)sym;                                                            \
            continue;                                                                             \
        }                                                                                         \
        cx_u32 len;                                                                               \
        int rep = 0;                                                                              \
        BIT(FAST, &d->is_rep[state], rep = 0, rep = 1);                                           \
        if (!rep) {                                                                               \
            d->rep3 = d->rep2;                                                                    \
            d->rep2 = d->rep1;                                                                    \
            d->rep1 = rep0;                                                                       \
            LENGTH(FAST, &d->match_len, pos_state, len);                                          \
            state = state < 7 ? 7 : 10;                                                           \
            DISTANCE(FAST, len);                                                                  \
            if (rep0 == 0xFFFFFFFFu) goto marker;                                                 \
            if ((cx_usize)rep0 >= upos || (cx_u64)rep0 >= reach) goto corrupt;                    \
        } else {                                                                                  \
            if (upos == 0) goto corrupt;                                                          \
            int g0 = 0;                                                                           \
            BIT(FAST, &d->is_rep_g0[state], g0 = 0, g0 = 1);                                      \
            if (!g0) {                                                                            \
                int full = 0;                                                                     \
                BIT(FAST, &d->is_rep0_long[state][pos_state], full = 0, full = 1);                \
                if (!full) {                                                                      \
                    /* A short rep: one byte from the last distance. */                          \
                    state = state < 7 ? 9 : 11;                                                   \
                    prev = base[upos - rep0 - 1];                                                 \
                    base[upos++] = (cx_u8)prev;                                                   \
                    continue;                                                                     \
                }                                                                                 \
            } else {                                                                              \
                cx_u32 dist;                                                                      \
                int g1 = 0;                                                                       \
                BIT(FAST, &d->is_rep_g1[state], g1 = 0, g1 = 1);                                  \
                if (!g1) {                                                                        \
                    dist = d->rep1;                                                               \
                } else {                                                                          \
                    int g2 = 0;                                                                   \
                    BIT(FAST, &d->is_rep_g2[state], g2 = 0, g2 = 1);                              \
                    if (!g2) {                                                                    \
                        dist = d->rep2;                                                           \
                    } else {                                                                      \
                        dist = d->rep3;                                                           \
                        d->rep3 = d->rep2;                                                        \
                    }                                                                             \
                    d->rep2 = d->rep1;                                                            \
                }                                                                                 \
                d->rep1 = rep0;                                                                   \
                rep0 = dist;                                                                      \
            }                                                                                     \
            LENGTH(FAST, &d->rep_len, pos_state, len);                                            \
            state = state < 7 ? 8 : 11;                                                           \
        }                                                                                         \
        len += 2;                                                                                 \
        if (len > ulimit - upos) goto corrupt;                                                    \
        copy_match(base, upos, (cx_usize)rep0 + 1, len, ulimit - upos);                           \
        upos += len;                                                                              \
        prev = base[upos - 1];                                                                    \
    }

/* Copies a match of `len` bytes from `dist` back, with `room` bytes free from where it goes, at
 * least `len`. Where 15 more are free, it's copied 16 bytes at a time, each 16 read before any is
 * written, as long as each 16 reads only bytes before the match or 16 or more back: what it writes
 * past the match is the next symbols' to write over, and comes before where the text ends, so no
 * other block's. Most matches in text are short, so one 16-byte copy takes the place of a branch on
 * the length that guesses wrong much of the time, as liblzma's 32-byte copies do. */
static inline __attribute__((always_inline)) void copy_match(cx_u8* out, cx_usize pos, cx_usize dist, cx_u32 len, cx_usize room) {
    const cx_u8* from = out + pos - dist;
    cx_u8* to = out + pos;
    if (__builtin_expect(room - len >= 15 && dist >= (len < 16 ? len : 16), 1)) {
        cx_u32 i = 0;
        do {
            __builtin_memmove(to + i, from + i, 16);
            i += 16;
        } while (i < len);
        return;
    }
    if (dist >= len) {
        /* From text before the copy: whole moves, the last one ending where the match does, so
         * nothing reads what this copy writes. */
        if (len >= 16) {
            cx_u32 i = 0;
            for (; i + 16 < len; i += 16) memcpy(to + i, from + i, 16);
            memcpy(to + len - 16, from + len - 16, 16);
        } else if (len >= 8) {
            cx_u64 a, b;
            memcpy(&a, from, 8);
            memcpy(&b, from + len - 8, 8);
            memcpy(to, &a, 8);
            memcpy(to + len - 8, &b, 8);
        } else if (len >= 4) {
            cx_u32 a, b;
            memcpy(&a, from, 4);
            memcpy(&b, from + len - 4, 4);
            memcpy(to, &a, 4);
            memcpy(to + len - 4, &b, 4);
        } else {
            cx_u8 a = from[0], b = from[1], c = from[len - 1];
            to[0] = a, to[1] = b, to[len - 1] = c;
        }
    } else if (dist == 1) {
        memset(to, from[0], len);
    } else {
        for (cx_u32 i = 0; i < len; i++) to[i] = from[i];
    }
}

/* Decodes LZMA symbols from in[d->at..n) into out from d->pos until it reaches `stop`. A match may
 * run past `stop` but not past `limit`, where the text ends, or it's corrupt. Gives 0 at `stop`; 1
 * at an end marker, if `eopm` allows one; -1 for data that's corrupt; -2 when the input ends first.
 * With `partial`, it gives 2 instead where fewer than FAST_INPUT bytes are left, between symbols,
 * with all it needs to carry on stored. d->pos and d->at say how far it got. */
static inline __attribute__((always_inline)) int lzma_run_with(dec_t* d, const cx_u8* in_base, cx_usize n, cx_u8* out, cx_usize stop, cx_usize limit, int eopm, int partial, const cx_u32 lc, const cx_usize lit_mask, const cx_usize pb_mask) {
    cx_u32 range = d->range, code = d->code;
    const cx_u8* in = in_base + d->at;
    const cx_u8* end = in_base + n;
    /* While `in` is before this, FAST_INPUT bytes are left. */
    const cx_u8* fast_end = n >= FAST_INPUT ? end - (FAST_INPUT - 1) : in_base;
    cx_u8* const base = out + d->start;
    cx_usize upos = d->pos - d->start;
    const cx_usize ustop = stop - d->start, ulimit = limit - d->start;
    const cx_u64 reach = d->dict;
    cx_u32 state = d->state, rep0 = d->rep0;
    cx_u32 prev = upos > 0 ? base[upos - 1] : 0;
    int status = 0;
    for (;;) {
        while (upos < ustop && in < fast_end) SYMBOL(1)
        if (upos >= ustop) break;
        if (partial) goto pause;
        SYMBOL(0)
    }
finish:
    /* The range decoder takes its last byte as it would before another bit, as the encoder wrote
     * it, so the data is used up exactly. */
    if (range < TOP) {
        if (in == end) goto short_input;
        range <<= 8;
        code = (code << 8) | *in++;
    }
    d->state = state, d->rep0 = rep0;
    d->range = range, d->code = code;
    d->at = (cx_usize)(in - in_base);
    d->pos = d->start + upos;
    return status;
pause:
    /* Each bit takes in the byte it needs first, so the range decoder is as it would be before the
     * next symbol. */
    d->state = state, d->rep0 = rep0;
    d->range = range, d->code = code;
    d->at = (cx_usize)(in - in_base);
    d->pos = d->start + upos;
    return 2;
marker:
    if (!eopm) goto corrupt;
    status = 1;
    goto finish;
corrupt:
    d->pos = d->start + upos;
    d->at = (cx_usize)(in - in_base);
    return -1;
short_input:
    d->pos = d->start + upos;
    d->at = n;
    return -2;
}

/* Every xz preset writes lc=3, lp=0 and pb=2, and with those as constants the loop has three
 * values fewer to keep in registers, and finds a literal's probabilities with a constant mask and
 * shift. Other properties take the same loop with them as values. */
static int lzma_run(dec_t* d, const cx_u8* in_base, cx_usize n, cx_u8* out, cx_usize stop, cx_usize limit, int eopm, int partial) {
    if (d->lc == 3 && d->lp == 0 && d->pb == 2) {
        return lzma_run_with(d, in_base, n, out, stop, limit, eopm, partial, 3, 0x100 - (0x100 >> 3), 3);
    }
    return lzma_run_with(d, in_base, n, out, stop, limit, eopm, partial, d->lc,
                         ((cx_usize)0x100 << d->lp) - ((cx_usize)0x100 >> d->lc), ((cx_usize)1 << d->pb) - 1);
}

/* At the end of a .lzma file's text, where its header said it ends: an end marker may follow, as
 * liblzma takes since xz 5.4, but nothing else. 0 for the marker, -1 for anything else, -2 when
 * the input ends first. */
static int lzma_marker(dec_t* d, const cx_u8* in_base, cx_usize n) {
    cx_u32 range = d->range, code = d->code;
    const cx_u8* in = in_base + d->at;
    const cx_u8* end = in_base + n;
    cx_u32 state = d->state, rep0 = d->rep0;
    const cx_usize pos_state = (d->pos - d->start) & (((cx_usize)1 << d->pb) - 1);
    int bit = 0;
    BIT(0, &d->is_match[state][pos_state], bit = 0, bit = 1);
    if (!bit) goto corrupt;
    BIT(0, &d->is_rep[state], bit = 0, bit = 1);
    if (bit) goto corrupt;
    cx_u32 len;
    LENGTH(0, &d->match_len, pos_state, len);
    DISTANCE(0, len);
    if (rep0 != 0xFFFFFFFFu) goto corrupt;
    if (range < TOP) {
        if (in == end) goto short_input;
        range <<= 8;
        code = (code << 8) | *in++;
    }
    d->range = range, d->code = code;
    d->at = (cx_usize)(in - in_base);
    return 0;
corrupt:
    return -1;
short_input:
    return -2;
}

/* --- LZMA2 ----------------------------------------------------------------------------------- */

cx_i32 cx_xz_lzma2_size(const cx_u8* in, cx_usize n, cx_u64* info) {
    cx_usize at = 0;
    cx_u64 total = 0;
    cx_i32 status;
    for (;;) {
        if (at >= n) {
            status = -2;
            break;
        }
        cx_u32 c = in[at];
        if (c == 0) {
            at++;
            status = 0;
            break;
        }
        if (c >= 0x80) {
            if (n - at < 5) {
                status = -2;
                break;
            }
            total += ((cx_u64)(c & 0x1F) << 16 | (cx_u64)in[at + 1] << 8 | in[at + 2]) + 1;
            cx_usize packed = ((cx_usize)in[at + 3] << 8 | in[at + 4]) + 1;
            cx_usize head = c >= 0xC0 ? 6 : 5;
            if (n - at < head || n - at - head < packed) {
                status = -2;
                break;
            }
            at += head + packed;
        } else if (c <= 2) {
            if (n - at < 3) {
                status = -2;
                break;
            }
            cx_usize un = ((cx_usize)in[at + 1] << 8 | in[at + 2]) + 1;
            total += un;
            if (n - at - 3 < un) {
                status = -2;
                break;
            }
            at += 3 + un;
        } else {
            status = -1;
            break;
        }
    }
    info[0] = total;
    info[1] = at;
    return status;
}

void cx_xz_lzma2_start(cx_u8* ctx, cx_u64 dict, cx_usize start) {
    dec_t* d = (dec_t*)ctx;
    d->dict = dict_reach(dict);
    d->lc = d->lp = 0;
    d->pos = d->start = start;
    d->end = ~(cx_u64)0;
    d->at = 0;
    d->phase = 3; /* it needs properties, and a dictionary reset */
    d->partial = 0;
}

cx_i32 cx_xz_lzma2(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize cap) {
    dec_t* d = (dec_t*)ctx;
    for (;;) {
        cx_usize at = d->at;
        if (at >= n) return -2;
        cx_u32 c = in[at];
        if (c == 0) {
            d->at = at + 1;
            return 0;
        }
        if (c >= 0xE0 || c == 1) {
            d->phase = 1;
            d->start = d->pos;
        } else if (d->phase & 2) {
            return -1;
        }
        if (c >= 0x80) {
            if (c < 0xC0 && (d->phase & 1)) return -1;
            if (n - at < 5) return -2;
            cx_usize un = ((cx_usize)(c & 0x1F) << 16 | (cx_usize)in[at + 1] << 8 | in[at + 2]) + 1;
            cx_usize packed = ((cx_usize)in[at + 3] << 8 | in[at + 4]) + 1;
            /* Nothing has changed yet that doing this chunk again would change. */
            if (cap - d->pos < un) return 1;
            at += 5;
            if (d->partial && n - at < (cx_usize)(c >= 0xC0) + packed) return -3;
            if (c >= 0xC0) {
                if (at >= n) return -2;
                if (lzma_props(d, in[at++]) != 0) return -1;
                d->phase = 0;
                lzma_reset(d);
            } else if (c >= 0xA0) {
                lzma_reset(d);
            }
            /* The chunk's range decoder sees only the chunk's bytes. */
            cx_usize avail = n - at < packed ? n - at : packed;
            d->at = at;
            int e = rc_init(d, in, at + avail);
            if (e == 0) e = lzma_run(d, in, at + avail, out, d->pos + un, d->pos + un, 0, 0);
            /* When the chunk's bytes run out, the data was cut short, or the chunk is corrupt. */
            if (e == -2) return avail < packed ? -2 : -1;
            if (e != 0 || d->code != 0 || d->at != at + packed) return -1;
            d->at = at + packed;
        } else if (c <= 2) {
            if (n - at < 3) return -2;
            cx_usize un = ((cx_usize)in[at + 1] << 8 | in[at + 2]) + 1;
            if (cap - d->pos < un) return 1;
            at += 3;
            if (n - at < un) return -2;
            memcpy(out + d->pos, in + at, un);
            d->pos += un;
            d->at = at + un;
        } else {
            return -1;
        }
    }
}

/* --- .lzma ------------------------------------------------------------------------------------ */

cx_i32 cx_xz_lzma1_start(cx_u8* ctx, cx_u32 props, cx_u64 dict, cx_u64 size, cx_usize start) {
    dec_t* d = (dec_t*)ctx;
    d->lc = d->lp = 0;
    if (lzma_props(d, props) != 0) return -1;
    lzma_reset(d);
    d->dict = dict_reach(dict);
    d->pos = d->start = start;
    d->end = size == ~(cx_u64)0 ? size : start + size;
    d->at = 0;
    d->phase = 0;
    d->partial = 0;
    return 0;
}

cx_i32 cx_xz_lzma1(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize cap) {
    dec_t* d = (dec_t*)ctx;
    if (d->phase == 0) {
        int e = rc_init(d, in, n);
        if (e != 0) return e;
        d->phase = 1;
    }
    /* With the room to the end, decode to it exactly. With less, stop where any match still fits,
     * and ask for more. */
    int whole = d->end <= (cx_u64)cap;
    cx_usize limit = whole ? (cx_usize)d->end : cap;
    cx_usize stop = whole ? limit : (cap - d->pos > MATCH_MAX ? cap - MATCH_MAX : d->pos);
    if (!whole && stop <= d->pos) return 1;
    int e = lzma_run(d, in, n, out, stop, limit, 1, (int)d->partial);
    if (e < 0 || e == 2) return e;
    if (e == 1) {
        /* The end marker, where the header's size says, if it gave one. */
        if (d->code != 0) return -1;
        if (d->end != ~(cx_u64)0 && (cx_u64)d->pos != d->end) return -1;
        return 0;
    }
    if (!whole) return 1;
    /* At the size the header gave: done if the range decoder is, or an end marker follows. */
    if (d->code == 0) return 0;
    if (d->partial && n - d->at < FAST_INPUT) return 2;
    e = lzma_marker(d, in, n);
    if (e < 0) return e;
    return d->code == 0 ? 0 : -1;
}

cx_usize cx_xz_len(const cx_u8* ctx) { return ((const dec_t*)ctx)->pos; }
cx_usize cx_xz_used(const cx_u8* ctx) { return ((const dec_t*)ctx)->at; }

/* --- a piece at a time ------------------------------------------------------------------------ */

/* std.xz's Decompressor gives the decoders the input from where they stopped, so they count it
 * from 0 again, and says whether more may come. */
void cx_xz_resume(cx_u8* ctx, cx_i32 partial) {
    dec_t* d = (dec_t*)ctx;
    d->at = 0;
    d->partial = partial != 0;
}

/* Moves what a decoder still needs to the front of its output, `buf`: the text matches can reach
 * back to, and everything from `keep` on. Gives how far it moved. Positions count from the last
 * dictionary reset, and only their low four bits pick probabilities, so once there's more text
 * since the reset than matches reach, the reset may as well have been within 16 bytes of the
 * front, which keeps the text that's left in the buffer. */
cx_usize cx_xz_slide(cx_u8* ctx, cx_u8* buf, cx_usize keep) {
    dec_t* d = (dec_t*)ctx;
    const cx_usize upos = d->pos - d->start;
    const cx_u64 need = d->dict + 16;
    const cx_usize from = (cx_u64)upos <= need ? d->start : d->pos - (cx_usize)need;
    const cx_usize shift = keep < from ? keep : from;
    if (shift == 0) return 0;
    memmove(buf, buf + shift, d->pos - shift);
    d->pos -= shift;
    d->start = shift <= d->start ? d->start - shift : (d->start - shift) & 15;
    if (d->end != ~(cx_u64)0) d->end -= shift;
    return shift;
}

/* --- the branch filters ------------------------------------------------------------------------ */

void cx_xz_delta(cx_u8* buf, cx_usize n, cx_u32 dist) {
    for (cx_usize i = dist; i < n; i++) buf[i] = (cx_u8)(buf[i] + buf[i - dist]);
}

static cx_u32 get32le(const cx_u8* p) { return (cx_u32)p[0] | (cx_u32)p[1] << 8 | (cx_u32)p[2] << 16 | (cx_u32)p[3] << 24; }
static void put32le(cx_u8* p, cx_u32 v) { p[0] = (cx_u8)v, p[1] = (cx_u8)(v >> 8), p[2] = (cx_u8)(v >> 16), p[3] = (cx_u8)(v >> 24); }
static cx_u32 get32be(const cx_u8* p) { return (cx_u32)p[0] << 24 | (cx_u32)p[1] << 16 | (cx_u32)p[2] << 8 | p[3]; }

/* x86: E8 and E9, call and jump, take a 32-bit offset, which the encoder made absolute so that
 * calls to one place look alike. */
#define X86_MSBYTE(b) ((((b) + 1) & 0xFE) == 0)

static cx_usize bcj_x86(cx_u8* buf, cx_usize n, cx_u32 now, cx_u32* st) {
    static const cx_u32 bit_number[8] = {0, 1, 2, 2, 3, 3, 3, 3};
    cx_u32 prev_mask = st[0], prev_pos = st[1];
    if (n < 5) return 0;
    cx_usize i = 0;
    while (i <= n - 5) {
        cx_u8 b = buf[i];
        if (b != 0xE8 && b != 0xE9) {
            i++;
            continue;
        }
        cx_u32 offset = now + (cx_u32)i - prev_pos;
        prev_pos = now + (cx_u32)i;
        if (offset > 5) {
            prev_mask = 0;
        } else {
            for (cx_u32 k = 0; k < offset; k++) {
                prev_mask &= 0x77;
                prev_mask <<= 1;
            }
        }
        b = buf[i + 4];
        if (X86_MSBYTE(b) && (prev_mask >> 1) <= 4 && (prev_mask >> 1) != 3) {
            cx_u32 src = (cx_u32)b << 24 | (cx_u32)buf[i + 3] << 16 | (cx_u32)buf[i + 2] << 8 | buf[i + 1];
            cx_u32 dest;
            for (;;) {
                dest = src - (now + (cx_u32)i + 5);
                if (prev_mask == 0) break;
                cx_u32 k = bit_number[prev_mask >> 1];
                b = (cx_u8)(dest >> (24 - k * 8));
                if (!X86_MSBYTE(b)) break;
                src = dest ^ ((1u << (32 - k * 8)) - 1);
            }
            buf[i + 4] = (cx_u8)(~(((dest >> 24) & 1) - 1));
            buf[i + 3] = (cx_u8)(dest >> 16);
            buf[i + 2] = (cx_u8)(dest >> 8);
            buf[i + 1] = (cx_u8)dest;
            i += 5;
            prev_mask = 0;
        } else {
            i++;
            prev_mask |= 1;
            if (X86_MSBYTE(b)) prev_mask |= 0x10;
        }
    }
    st[0] = prev_mask, st[1] = prev_pos;
    return i;
}

static cx_usize bcj_powerpc(cx_u8* buf, cx_usize n, cx_u32 now) {
    cx_usize i = 0;
    for (; i + 4 <= n; i += 4) {
        if ((buf[i] >> 2) == 0x12 && (buf[i + 3] & 3) == 1) {
            cx_u32 src = ((cx_u32)buf[i] & 3) << 24 | (cx_u32)buf[i + 1] << 16 | (cx_u32)buf[i + 2] << 8 | ((cx_u32)buf[i + 3] & ~3u);
            cx_u32 dest = src - (now + (cx_u32)i);
            buf[i] = (cx_u8)(0x48 | ((dest >> 24) & 3));
            buf[i + 1] = (cx_u8)(dest >> 16);
            buf[i + 2] = (cx_u8)(dest >> 8);
            buf[i + 3] = (cx_u8)((buf[i + 3] & 3) | (cx_u8)dest);
        }
    }
    return i;
}

static cx_usize bcj_ia64(cx_u8* buf, cx_usize n, cx_u32 now) {
    static const cx_u32 branch_table[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
    cx_usize i = 0;
    for (; i + 16 <= n; i += 16) {
        cx_u32 mask = branch_table[buf[i] & 0x1F];
        cx_u32 bit_pos = 5;
        for (cx_u32 slot = 0; slot < 3; slot++, bit_pos += 41) {
            if (((mask >> slot) & 1) == 0) continue;
            cx_usize byte_pos = bit_pos >> 3;
            cx_u32 bit_res = bit_pos & 7;
            cx_u64 instr = 0;
            for (cx_u32 j = 0; j < 6; j++) instr += (cx_u64)buf[i + j + byte_pos] << (8 * j);
            cx_u64 norm = instr >> bit_res;
            if (((norm >> 37) & 0xF) == 0x5 && ((norm >> 9) & 0x7) == 0) {
                cx_u32 src = (cx_u32)((norm >> 13) & 0xFFFFF);
                src |= (cx_u32)((norm >> 36) & 1) << 20;
                src <<= 4;
                cx_u32 dest = src - (now + (cx_u32)i);
                dest >>= 4;
                norm &= ~((cx_u64)0x8FFFFF << 13);
                norm |= (cx_u64)(dest & 0xFFFFF) << 13;
                norm |= (cx_u64)(dest & 0x100000) << (36 - 20);
                instr &= (1u << bit_res) - 1;
                instr |= norm << bit_res;
                for (cx_u32 j = 0; j < 6; j++) buf[i + j + byte_pos] = (cx_u8)(instr >> (8 * j));
            }
        }
    }
    return i;
}

static cx_usize bcj_arm(cx_u8* buf, cx_usize n, cx_u32 now) {
    cx_usize i = 0;
    for (; i + 4 <= n; i += 4) {
        if (buf[i + 3] == 0xEB) {
            cx_u32 src = (cx_u32)buf[i + 2] << 16 | (cx_u32)buf[i + 1] << 8 | buf[i];
            src <<= 2;
            cx_u32 dest = src - (now + (cx_u32)i + 8);
            dest >>= 2;
            buf[i + 2] = (cx_u8)(dest >> 16);
            buf[i + 1] = (cx_u8)(dest >> 8);
            buf[i] = (cx_u8)dest;
        }
    }
    return i;
}

static cx_usize bcj_armthumb(cx_u8* buf, cx_usize n, cx_u32 now) {
    cx_usize i = 0;
    for (; i + 4 <= n; i += 2) {
        if ((buf[i + 1] & 0xF8) == 0xF0 && (buf[i + 3] & 0xF8) == 0xF8) {
            cx_u32 src = ((cx_u32)buf[i + 1] & 7) << 19 | (cx_u32)buf[i] << 11 | ((cx_u32)buf[i + 3] & 7) << 8 | buf[i + 2];
            src <<= 1;
            cx_u32 dest = src - (now + (cx_u32)i + 4);
            dest >>= 1;
            buf[i + 1] = (cx_u8)(0xF0 | ((dest >> 19) & 7));
            buf[i] = (cx_u8)(dest >> 11);
            buf[i + 3] = (cx_u8)(0xF8 | ((dest >> 8) & 7));
            buf[i + 2] = (cx_u8)dest;
            i += 2;
        }
    }
    return i;
}

static cx_usize bcj_sparc(cx_u8* buf, cx_usize n, cx_u32 now) {
    cx_usize i = 0;
    for (; i + 4 <= n; i += 4) {
        if ((buf[i] == 0x40 && (buf[i + 1] & 0xC0) == 0) || (buf[i] == 0x7F && (buf[i + 1] & 0xC0) == 0xC0)) {
            cx_u32 src = get32be(buf + i);
            src <<= 2;
            cx_u32 dest = src - (now + (cx_u32)i);
            dest >>= 2;
            dest = (((0 - ((dest >> 22) & 1)) << 22) & 0x3FFFFFFF) | (dest & 0x3FFFFF) | 0x40000000;
            buf[i] = (cx_u8)(dest >> 24);
            buf[i + 1] = (cx_u8)(dest >> 16);
            buf[i + 2] = (cx_u8)(dest >> 8);
            buf[i + 3] = (cx_u8)dest;
        }
    }
    return i;
}

static cx_usize bcj_arm64(cx_u8* buf, cx_usize n, cx_u32 now) {
    cx_usize i = 0;
    for (; i + 4 <= n; i += 4) {
        cx_u32 pc = now + (cx_u32)i;
        cx_u32 instr = get32le(buf + i);
        if ((instr >> 26) == 0x25) {
            /* BL */
            cx_u32 src = instr;
            instr = 0x94000000;
            pc >>= 2;
            pc = 0u - pc;
            instr |= (src + pc) & 0x03FFFFFF;
            put32le(buf + i, instr);
        } else if ((instr & 0x9F000000) == 0x90000000) {
            /* ADRP, within 512 MiB */
            cx_u32 src = ((instr >> 29) & 3) | ((instr >> 3) & 0x001FFFFC);
            if ((src + 0x00020000) & 0x001C0000) continue;
            instr &= 0x9000001F;
            pc >>= 12;
            pc = 0u - pc;
            cx_u32 dest = src + pc;
            instr |= (dest & 3) << 29;
            instr |= (dest & 0x0003FFFC) << 3;
            instr |= (0u - (dest & 0x00020000)) & 0x00E00000;
            put32le(buf + i, instr);
        }
    }
    return i;
}

/* RISC-V: JAL, and AUIPC with the instruction after it, which the encoder rewrote. */
static cx_usize bcj_riscv(cx_u8* buf, cx_usize n, cx_u32 now) {
    if (n < 8) return 0;
    cx_usize last = n - 8, i = 0;
    for (; i <= last; i += 2) {
        cx_u32 inst = buf[i];
        if (inst == 0xEF) {
            cx_u32 b1 = buf[i + 1];
            if ((b1 & 0x0D) != 0) continue;
            cx_u32 b2 = buf[i + 2], b3 = buf[i + 3];
            cx_u32 pc = now + (cx_u32)i;
            cx_u32 addr = ((b1 & 0xF0) << 13) | (b2 << 9) | (b3 << 1);
            addr -= pc;
            buf[i + 1] = (cx_u8)((b1 & 0x0F) | ((addr >> 8) & 0xF0));
            buf[i + 2] = (cx_u8)(((addr >> 16) & 0x0F) | ((addr >> 7) & 0x10) | ((addr << 4) & 0xE0));
            buf[i + 3] = (cx_u8)(((addr >> 4) & 0x7F) | ((addr >> 13) & 0x80));
            i += 4 - 2;
        } else if ((inst & 0x7F) == 0x17) {
            cx_u32 inst2;
            inst |= (cx_u32)buf[i + 1] << 8;
            inst |= (cx_u32)buf[i + 2] << 16;
            inst |= (cx_u32)buf[i + 3] << 24;
            if (inst & 0xE80) {
                /* AUIPC's rd isn't x0 or x2: the encoder left it, unless it's a pair it would
                 * have rewritten, which it marks this way. */
                inst2 = get32le(buf + i + 4);
                if ((((inst << 8) ^ (inst2 - 3)) & 0xF8003) != 0) {
                    i += 6 - 2;
                    continue;
                }
                cx_u32 addr = (inst & 0xFFFFF000) + (inst2 >> 20);
                inst = 0x17 | (2 << 7) | (inst2 << 12);
                inst2 = addr;
            } else {
                /* rd is x0 or x2: a pair the encoder rewrote. */
                cx_u32 inst2_rs1 = inst >> 27;
                if ((cx_u32)((inst - 0x3117) << 18) >= (inst2_rs1 & 0x1D)) {
                    i += 4 - 2;
                    continue;
                }
                cx_u32 addr = get32be(buf + i + 4);
                addr -= now + (cx_u32)i;
                inst2 = (inst >> 12) | (addr << 20);
                inst = 0x17 | (inst2_rs1 << 7) | ((addr + 0x800) & 0xFFFFF000);
            }
            put32le(buf + i, inst);
            put32le(buf + i + 4, inst2);
            i += 8 - 2;
        }
    }
    return i;
}

/* Each filter gives where it stopped: what's before that is done, and nothing from there on has
 * changed, so it can carry on from there when more comes. x86 carries two numbers over too. */
static int bcj_run(cx_u32 id, cx_u8* buf, cx_usize n, cx_u32 now, cx_u32* st, cx_usize* done) {
    switch (id) {
        case 4: *done = bcj_x86(buf, n, now, st); return 0;
        case 5: *done = bcj_powerpc(buf, n, now); return 0;
        case 6: *done = bcj_ia64(buf, n, now); return 0;
        case 7: *done = bcj_arm(buf, n, now); return 0;
        case 8: *done = bcj_armthumb(buf, n, now); return 0;
        case 9: *done = bcj_sparc(buf, n, now); return 0;
        case 10: *done = bcj_arm64(buf, n, now); return 0;
        case 11: *done = bcj_riscv(buf, n, now); return 0;
        default: return -1;
    }
}

cx_i32 cx_xz_bcj(cx_u32 id, cx_u8* buf, cx_usize n, cx_u32 start) {
    cx_u32 st[2] = {0, start - 5};
    cx_usize done;
    return bcj_run(id, buf, n, start, st, &done);
}

/* A block's filters before LZMA2, undone a piece at a time for std.xz's Decompressor, in the order
 * they're undone: the one next to LZMA2 first. Each has its ID and its start offset or distance,
 * how far into the block it has got, x86's two numbers, and the last 256 bytes delta gave. */
typedef struct {
    cx_u32 id, arg;
    cx_u64 done;
    cx_u32 st[2];
    cx_u8 hist[256];
} stage_t;

typedef struct {
    cx_u32 count;
    stage_t s[3];
} chain_t;

_Static_assert(sizeof(chain_t) <= CX_XZ_CHAIN, "CX_XZ_CHAIN is too small");

void cx_xz_chain_start(cx_u8* ctx, const cx_u64* filters, cx_u32 count) {
    chain_t* c = (chain_t*)ctx;
    memset(c, 0, sizeof *c);
    c->count = count;
    for (cx_u32 k = 0; k < count; k++) {
        stage_t* s = &c->s[k];
        s->id = (cx_u32)filters[2 * (count - 1 - k)];
        s->arg = (cx_u32)filters[2 * (count - 1 - k) + 1];
        s->st[1] = s->arg - 5;
    }
}

/* One filter over buf[0..n), from `at` into the block: gives how much of it is done, which is all
 * of it at the block's end. Delta adds the byte `dist` back as it gave it, from its history. */
static cx_usize stage_run(stage_t* s, cx_u8* buf, cx_usize n, cx_u64 at, int last) {
    if (s->id == 3) {
        const cx_u32 dist = s->arg;
        for (cx_usize i = 0; i < n; i++) {
            const cx_u8 v = (cx_u8)(buf[i] + s->hist[(cx_u8)(at + i - dist)]);
            s->hist[(cx_u8)(at + i)] = v;
            buf[i] = v;
        }
        return n;
    }
    cx_usize done = 0;
    bcj_run(s->id, buf, n, s->arg + (cx_u32)at, s->st, &done);
    return last ? n : done;
}

/* Undoes the chain over buf[0..n), which is from `at` into the block: what has come out of LZMA2
 * and isn't through every filter yet. Each filter goes as far as the one before it has. Gives how
 * much of buf is through them all; `last` says the block ends at n. */
cx_usize cx_xz_chain(cx_u8* ctx, cx_u8* buf, cx_usize n, cx_u64 at, cx_i32 last) {
    chain_t* c = (chain_t*)ctx;
    cx_u64 end = at + n;
    for (cx_u32 k = 0; k < c->count; k++) {
        stage_t* s = &c->s[k];
        if (end > s->done) s->done += stage_run(s, buf + (s->done - at), (cx_usize)(end - s->done), s->done, last != 0);
        end = s->done;
    }
    return (cx_usize)(end - at);
}

/* --- CRC-64 ------------------------------------------------------------------------------------ */

/* ECMA-182's CRC-64, reflected, as .xz's checks use it. With carry-less multiplies, it folds 16
 * bytes at a time: a 16-byte block followed by 16 more has the CRC of the block times x^128, mod
 * the polynomial, added to them, and that product is the block's two halves times x^191 and x^127
 * mod the polynomial, reflected, which a 64-bit carry-less multiply each gives. The last 16 bytes
 * go through the tables, which take 8 bytes at a time otherwise. */
static cx_u64 crc64_table[8][256];
static int crc64_ready;

static void crc64_init(void) {
    for (cx_u32 i = 0; i < 256; i++) {
        cx_u64 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1;
        crc64_table[0][i] = c;
    }
    for (cx_u32 i = 0; i < 256; i++) {
        for (int t = 1; t < 8; t++) {
            cx_u64 c = crc64_table[t - 1][i];
            crc64_table[t][i] = crc64_table[0][c & 0xFF] ^ (c >> 8);
        }
    }
    __atomic_store_n(&crc64_ready, 1, __ATOMIC_RELEASE);
}

/* The CRC's register, before it's inverted, carried over the bytes by the tables. */
static cx_u64 crc64_tables(cx_u64 crc, const cx_u8* p, cx_usize n) {
    while (n >= 8) {
        cx_u64 v;
        memcpy(&v, p, 8);
        v ^= crc;
        crc = crc64_table[7][v & 0xFF] ^ crc64_table[6][(v >> 8) & 0xFF] ^ crc64_table[5][(v >> 16) & 0xFF] ^
              crc64_table[4][(v >> 24) & 0xFF] ^ crc64_table[3][(v >> 32) & 0xFF] ^ crc64_table[2][(v >> 40) & 0xFF] ^
              crc64_table[1][(v >> 48) & 0xFF] ^ crc64_table[0][v >> 56];
        p += 8;
        n -= 8;
    }
    while (n--) crc = crc64_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>

/* Whether the processor has PCLMULQDQ. It's asked once, as cpuid traps out to a hypervisor and
 * costs about a microsecond. */
static int clmul_ok(void) {
    static int known = -1; /* threads that race here store the same answer */
    int k = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (k < 0) {
        unsigned a, b, c, d;
        k = __get_cpuid(1, &a, &b, &c, &d) && (c >> 1 & 1);
        __atomic_store_n(&known, k, __ATOMIC_RELAXED);
    }
    return k;
}

/* One 16-byte lane folded forward and added to `y`: the constants are x^(8d+63) and x^(8d-1)
 * modulo the polynomial, reflected, to fold it d bytes on. */
__attribute__((target("pclmul,sse4.1"))) static inline __m128i crc64_fold(__m128i x, __m128i k, __m128i y) {
    return _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x00), _mm_clmulepi64_si128(x, k, 0x11)), y);
}

/* The register over n bytes, n at least 64: four lanes folded 64 bytes at a time, as the multiplies
 * take several cycles each and one lane would wait on them, then folded into one. */
__attribute__((target("pclmul,sse4.1"))) static cx_u64 crc64_clmul(cx_u64 crc, const cx_u8* p, cx_usize n) {
    const __m128i k64 = _mm_set_epi64x((long long)0x081f6054a7842df4ull, (long long)0x6ae3efbb9dd441f3ull);
    const __m128i k48 = _mm_set_epi64x((long long)0x69a35d91c3730254ull, (long long)0xb5ea1af9c013aca4ull);
    const __m128i k32 = _mm_set_epi64x((long long)0x3be653a30fe1af51ull, (long long)0x60095b008a9efa44ull);
    const __m128i k16 = _mm_set_epi64x((long long)0xdabe95afc7875f40ull, (long long)0xe05dd497ca393ae4ull);
    __m128i a = _mm_xor_si128(_mm_loadu_si128((const __m128i*)p), _mm_cvtsi64_si128((long long)crc));
    __m128i b = _mm_loadu_si128((const __m128i*)(p + 16));
    __m128i c = _mm_loadu_si128((const __m128i*)(p + 32));
    __m128i d = _mm_loadu_si128((const __m128i*)(p + 48));
    p += 64;
    n -= 64;
    while (n >= 64) {
        a = crc64_fold(a, k64, _mm_loadu_si128((const __m128i*)p));
        b = crc64_fold(b, k64, _mm_loadu_si128((const __m128i*)(p + 16)));
        c = crc64_fold(c, k64, _mm_loadu_si128((const __m128i*)(p + 32)));
        d = crc64_fold(d, k64, _mm_loadu_si128((const __m128i*)(p + 48)));
        p += 64;
        n -= 64;
    }
    __m128i x = crc64_fold(a, k48, crc64_fold(b, k32, crc64_fold(c, k16, d)));
    while (n >= 16) {
        x = crc64_fold(x, k16, _mm_loadu_si128((const __m128i*)p));
        p += 16;
        n -= 16;
    }
    cx_u8 last[16];
    _mm_storeu_si128((__m128i*)last, x);
    return crc64_tables(crc64_tables(0, last, 16), p, n);
}
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>

/* CLX-281: the same folds with Arm64's PMULL, which comes with its AES instructions: pmull
 * multiplies the low halves and pmull2 the high ones, as pclmulqdq's 0x00 and 0x11 do. */
#define CX_PMULL 1
static inline uint64x2_t crc64_load(const cx_u8* p) { return vreinterpretq_u64_u8(vld1q_u8(p)); }

__attribute__((target("aes"))) static inline uint64x2_t crc64_fold(uint64x2_t x, uint64x2_t k, uint64x2_t y) {
    poly64x2_t px = vreinterpretq_p64_u64(x), pk = vreinterpretq_p64_u64(k);
    uint64x2_t lo = vreinterpretq_u64_p128(vmull_p64(vgetq_lane_p64(px, 0), vgetq_lane_p64(pk, 0)));
    uint64x2_t hi = vreinterpretq_u64_p128(vmull_high_p64(px, pk));
    return veorq_u64(veorq_u64(lo, hi), y);
}

__attribute__((target("aes"))) static cx_u64 crc64_pmull(cx_u64 crc, const cx_u8* p, cx_usize n) {
    static const cx_u64 ks[8] = {0x6ae3efbb9dd441f3ull, 0x081f6054a7842df4ull, 0xb5ea1af9c013aca4ull, 0x69a35d91c3730254ull,
                                 0x60095b008a9efa44ull, 0x3be653a30fe1af51ull, 0xe05dd497ca393ae4ull, 0xdabe95afc7875f40ull};
    const uint64x2_t k64 = vld1q_u64(ks), k48 = vld1q_u64(ks + 2), k32 = vld1q_u64(ks + 4), k16 = vld1q_u64(ks + 6);
    uint64x2_t a = veorq_u64(crc64_load(p), vcombine_u64(vcreate_u64(crc), vcreate_u64(0)));
    uint64x2_t b = crc64_load(p + 16), c = crc64_load(p + 32), d = crc64_load(p + 48);
    p += 64;
    n -= 64;
    while (n >= 64) {
        a = crc64_fold(a, k64, crc64_load(p));
        b = crc64_fold(b, k64, crc64_load(p + 16));
        c = crc64_fold(c, k64, crc64_load(p + 32));
        d = crc64_fold(d, k64, crc64_load(p + 48));
        p += 64;
        n -= 64;
    }
    uint64x2_t x = crc64_fold(a, k48, crc64_fold(b, k32, crc64_fold(c, k16, d)));
    while (n >= 16) {
        x = crc64_fold(x, k16, crc64_load(p));
        p += 16;
        n -= 16;
    }
    cx_u8 last[16];
    vst1q_u8(last, vreinterpretq_u8_u64(x));
    return crc64_tables(crc64_tables(0, last, 16), p, n);
}
#endif

cx_u64 cx_xz_crc64(cx_u64 crc, const cx_u8* p, cx_usize n) {
    if (!__atomic_load_n(&crc64_ready, __ATOMIC_ACQUIRE)) crc64_init();
#if defined(__x86_64__) || defined(_M_X64)
    if (n >= 64 && clmul_ok()) return ~crc64_clmul(~crc, p, n);
#elif defined(CX_PMULL)
    if (n >= 64 && (cx_arm_features() & CX_ARM_AES)) return ~crc64_pmull(~crc, p, n);
#endif
    return ~crc64_tables(~crc, p, n);
}

/* --- blocks at once ------------------------------------------------------------------------- */

/* Blocks whose headers give both sizes, as `xz -T` writes them, decode at once on `parallel
 * for`'s threads, each into its own part of the output: the data, the filters before LZMA2, the
 * padding and the check. Each block is CX_XZ_BLOCK numbers: where its data starts in the input,
 * the data's size, what it decompresses to, where that goes in the output, the dictionary size,
 * the check's ID, where the check starts, how many filters come before LZMA2, and for each of up
 * to three, its ID and its argument. */
typedef struct {
    const cx_u8* in;
    const cx_u64* desc;
    cx_u8* out;
    cx_u8* ok;
} blocks_job;

static int block_check(cx_u64 check, const cx_u8* p, cx_usize n, const cx_u8* want) {
    if (check == 1) return cx_zlib_crc32(0, p, n) == get32le(want);
    if (check == 4) return cx_xz_crc64(0, p, n) == ((cx_u64)get32le(want) | (cx_u64)get32le(want + 4) << 32);
    if (check == 10) {
        cx_u8 ctx[CX_DIGEST_CTX], digest[64];
        cx_digest_start(ctx, 2);
        cx_digest_add(ctx, p, n);
        cx_digest_finish(ctx, digest);
        return memcmp(digest, want, 32) == 0;
    }
    return 1;
}

static int block_decode(const blocks_job* j, cx_u64 k, dec_t* d) {
    const cx_u64* b = j->desc + k * CX_XZ_BLOCK;
    const cx_usize data = (cx_usize)b[0], packed = (cx_usize)b[1], unpacked = (cx_usize)b[2], at = (cx_usize)b[3];
    cx_xz_lzma2_start((cx_u8*)d, b[4], at);
    if (cx_xz_lzma2((cx_u8*)d, j->in + data, packed, j->out, at + unpacked) != 0) return 0;
    if (d->at != packed || d->pos != at + unpacked) return 0;
    for (cx_u64 f = b[7]; f-- > 0;) {
        if (b[8 + 2 * f] == 3) {
            cx_xz_delta(j->out + at, unpacked, (cx_u32)b[9 + 2 * f]);
        } else {
            cx_xz_bcj((cx_u32)b[8 + 2 * f], j->out + at, unpacked, (cx_u32)b[9 + 2 * f]);
        }
    }
    for (cx_usize i = data + packed; i < (cx_usize)b[6]; i++) {
        if (j->in[i] != 0) return 0;
    }
    return block_check(b[5], j->out + at, unpacked, j->in + b[6]);
}

static void blocks_body(void* v, cx_u64 begin, cx_u64 end) {
    const blocks_job* j = (const blocks_job*)v;
    dec_t* d = (dec_t*)cx_alloc(sizeof(dec_t));
    for (cx_u64 k = begin; k < end; k++) j->ok[k] = (cx_u8)block_decode(j, k, d);
    cx_free(d, sizeof(dec_t));
}

void cx_xz_blocks(const cx_u8* in, const cx_u64* desc, cx_usize n, cx_u8* out, cx_u8* ok) {
    blocks_job j = {in, desc, out, ok};
    cx_parallel((cx_u64)n, blocks_body, &j);
}
