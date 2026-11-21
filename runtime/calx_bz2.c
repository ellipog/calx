/* bzip2 decompression, behind std.bz2 (CLX-400): .bz2 files and .tar.bz2 archives, read as
 * Python's bz2.decompress reads them with libbzip2 1.0.8. Linked only into programs that call it.
 *
 * A stream is "BZh" and a digit, the block size in 100 kB, then blocks, then an end marker and a
 * CRC of the blocks' CRCs, with no byte alignment until the stream ends. A block is a 48-bit
 * marker, the CRC-32 of what it decodes to, a bit saying whether it's randomised, where the text
 * starts in the Burrows-Wheeler transform, which bytes it uses, 2 to 6 Huffman tables and which
 * one each 50 symbols use, then the symbols: the transform's output, move-to-front coded, with
 * runs of the front symbol written in base 2 by RUNA and RUNB. Undoing the transform gives the
 * text with each run of 4 to 255 bytes written as 4 of them and a count, which the output undoes.
 *
 * The decoder checks what libbzip2 checks, in the order it reads them, so broken data fails where
 * libbzip2 fails: a stream that ends early is BZ_TRUNC, as libbzip2 asks for more there, and a bad
 * header is BZ_FORMAT, as libbzip2 says BZ_DATA_ERROR_MAGIC. The output is resumable, since a
 * block can make up to 46 MB: cx_bz2_run stops when `out` is full and carries on when called again.
 *
 * Randomised blocks, which bzip2 before 0.9.5 could write, are undone with libbzip2's table of
 * numbers, below, whose notice follows:
 *
 * This program, "bzip2", the associated library "libbzip2", and all documentation, are copyright
 * (C) 1996-2019 Julian R Seward.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 *
 * 2. The origin of this software must not be misrepresented; you must not claim that you wrote
 *    the original software.  If you use this software in a product, an acknowledgment in the
 *    product documentation would be appreciated but is not required.
 *
 * 3. Altered source versions must be plainly marked as such, and must not be misrepresented as
 *    being the original software.
 *
 * 4. The name of the author may not be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Julian Seward, jseward@acm.org
 * bzip2/libbzip2 version 1.0.8 of 13 July 2019 */
#include "calx_rt.h"

#include <string.h>

/* What cx_bz2_run gives: the stream ended, `out` is full, or why it stopped. std.bz2 reads these. */
#define BZ_END 0
#define BZ_FULL 1
#define BZ_FORMAT (-1)
#define BZ_CORRUPT (-2)
#define BZ_CHECKSUM (-3)
#define BZ_TRUNC (-4)

#define GROUPS 6
#define ALPHA 258
#define CODE_LEN 23
#define MAX_SELECTORS 18002
/* Symbols whose code is at most this long are decoded by one look in a table. */
#define FAST_BITS 10
#define SLOW 0xffff
/* A perm entry no code reaches. */
#define NO_SYMBOL 0xffff

enum { PHASE_HEADER, PHASE_BLOCK, PHASE_OUTPUT, PHASE_DONE };

static const cx_u16 RNUMS[512] = {
    619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 985, 724, 205, 454, 863, 491, 741, 242, 949, 214, 733, 859, 335, 708, 621, 574,
    73,  654, 730, 472, 419, 436, 278, 496, 867, 210, 399, 680, 480, 51,  878, 465, 811, 169, 869, 675, 611, 697, 867, 561, 862, 687,
    507, 283, 482, 129, 807, 591, 733, 623, 150, 238, 59,  379, 684, 877, 625, 169, 643, 105, 170, 607, 520, 932, 727, 476, 693, 425,
    174, 647, 73,  122, 335, 530, 442, 853, 695, 249, 445, 515, 909, 545, 703, 919, 874, 474, 882, 500, 594, 612, 641, 801, 220, 162,
    819, 984, 589, 513, 495, 799, 161, 604, 958, 533, 221, 400, 386, 867, 600, 782, 382, 596, 414, 171, 516, 375, 682, 485, 911, 276,
    98,  553, 163, 354, 666, 933, 424, 341, 533, 870, 227, 730, 475, 186, 263, 647, 537, 686, 600, 224, 469, 68,  770, 919, 190, 373,
    294, 822, 808, 206, 184, 943, 795, 384, 383, 461, 404, 758, 839, 887, 715, 67,  618, 276, 204, 918, 873, 777, 604, 560, 951, 160,
    578, 722, 79,  804, 96,  409, 713, 940, 652, 934, 970, 447, 318, 353, 859, 672, 112, 785, 645, 863, 803, 350, 139, 93,  354, 99,
    820, 908, 609, 772, 154, 274, 580, 184, 79,  626, 630, 742, 653, 282, 762, 623, 680, 81,  927, 626, 789, 125, 411, 521, 938, 300,
    821, 78,  343, 175, 128, 250, 170, 774, 972, 275, 999, 639, 495, 78,  352, 126, 857, 956, 358, 619, 580, 124, 737, 594, 701, 612,
    669, 112, 134, 694, 363, 992, 809, 743, 168, 974, 944, 375, 748, 52,  600, 747, 642, 182, 862, 81,  344, 805, 988, 739, 511, 655,
    814, 334, 249, 515, 897, 955, 664, 981, 649, 113, 974, 459, 893, 228, 433, 837, 553, 268, 926, 240, 102, 654, 459, 51,  686, 754,
    806, 760, 493, 403, 415, 394, 687, 700, 946, 670, 656, 610, 738, 392, 760, 799, 887, 653, 978, 321, 576, 617, 626, 502, 894, 679,
    243, 440, 680, 879, 194, 572, 640, 724, 926, 56,  204, 700, 707, 151, 457, 449, 797, 195, 791, 558, 945, 679, 297, 59,  87,  824,
    713, 663, 412, 693, 342, 606, 134, 108, 571, 364, 631, 212, 174, 643, 304, 329, 343, 97,  430, 751, 497, 314, 983, 374, 822, 928,
    140, 206, 73,  263, 980, 736, 876, 478, 430, 305, 170, 514, 364, 692, 829, 82,  855, 953, 676, 246, 369, 970, 294, 750, 807, 827,
    150, 790, 288, 923, 804, 378, 215, 828, 592, 281, 565, 555, 710, 82,  896, 831, 547, 261, 524, 462, 293, 465, 502, 56,  661, 821,
    976, 991, 658, 869, 905, 758, 745, 193, 768, 550, 608, 933, 378, 286, 215, 979, 792, 961, 61,  688, 793, 644, 986, 403, 106, 366,
    905, 644, 372, 567, 466, 434, 645, 210, 389, 550, 919, 135, 780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 920, 176, 193, 713,
    857, 265, 203, 50,  668, 108, 645, 990, 626, 197, 510, 357, 358, 850, 858, 364, 936, 638,
};

typedef struct {
    /* The input: the next byte to read, and bits read ahead of it, MSB first, in the low nb of bb. */
    cx_usize pos;
    cx_u64 bb;
    int nb;
    int phase;
    cx_u32 level;
    /* The transform's vector: 100000 * level entries, from cx_alloc. */
    cx_u32* tt;
    cx_usize tt_cap;
    cx_u32 stored_crc, block_crc, combined_crc;
    int randomised;
    cx_i32 orig_ptr;
    /* The output: the next entry of tt, how many have been taken (the first counted), the byte
     * read ahead, and the run being written. */
    cx_u32 tpos;
    cx_i32 nblock, used;
    int k0, out_len, out_ch;
    int r_to_go, r_pos;
    cx_u32 crc_table[256];
    /* The block's tables. perm keeps what earlier blocks left, as libbzip2's does. */
    cx_i32 limit[GROUPS][CODE_LEN], base[GROUPS][CODE_LEN];
    cx_u16 perm[GROUPS][ALPHA];
    int min_len[GROUPS];
    cx_u16 fast[GROUPS][1 << FAST_BITS];
    cx_u8 len[GROUPS][ALPHA];
    cx_u8 selector[MAX_SELECTORS];
    cx_u8 seq_to_unseq[256];
    cx_i32 unzftab[256];
} bz_t;

cx_usize cx_bz2_ctx_size(void) { return sizeof(bz_t); }

void cx_bz2_init(cx_u8* ctx) {
    bz_t* s = (bz_t*)ctx;
    memset(s, 0, sizeof *s);
    for (int g = 0; g < GROUPS; g++) {
        for (int i = 0; i < ALPHA; i++) s->perm[g][i] = NO_SYMBOL;
    }
    for (cx_u32 i = 0; i < 256; i++) {
        cx_u32 c = i << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
        s->crc_table[i] = c;
    }
    s->phase = PHASE_DONE;
}

void cx_bz2_end(cx_u8* ctx) {
    bz_t* s = (bz_t*)ctx;
    if (s->tt) cx_free(s->tt, s->tt_cap * sizeof(cx_u32));
    s->tt = 0;
    s->tt_cap = 0;
}

/* Starts a stream at in[at]. */
void cx_bz2_start(cx_u8* ctx, cx_usize at) {
    bz_t* s = (bz_t*)ctx;
    s->pos = at;
    s->bb = 0;
    s->nb = 0;
    s->phase = PHASE_HEADER;
    s->combined_crc = 0;
}

/* Where the stream that ended ends: the byte after its last, as libbzip2's unused data starts. */
cx_usize cx_bz2_used(const cx_u8* ctx) {
    const bz_t* s = (const bz_t*)ctx;
    return s->pos - (cx_usize)(s->nb / 8);
}

/* --- Bits ---------------------------------------------------------------------------------- */

static inline void refill(bz_t* s, const cx_u8* in, cx_usize n) {
    if (s->pos + 8 <= n && s->nb <= 32) {
        /* Four bytes at once. */
        const cx_u8* p = in + s->pos;
        s->bb = s->bb << 32 | (cx_u64)p[0] << 24 | (cx_u64)p[1] << 16 | (cx_u64)p[2] << 8 | p[3];
        s->nb += 32;
        s->pos += 4;
    }
    while (s->nb <= 56 && s->pos < n) {
        s->bb = s->bb << 8 | in[s->pos++];
        s->nb += 8;
    }
}

static inline cx_u32 take(bz_t* s, int k) {
    s->nb -= k;
    return (cx_u32)(s->bb >> s->nb) & (cx_u32)(((cx_u64)1 << k) - 1);
}

/* k bits into v, or BZ_TRUNC from the function when the input has fewer. */
#define GET(v, k)                                     \
    do {                                              \
        if (s->nb < (k)) {                            \
            refill(s, in, n);                         \
            if (s->nb < (k)) return BZ_TRUNC;         \
        }                                             \
        (v) = take(s, (k));                           \
    } while (0)

/* --- Huffman tables ------------------------------------------------------------------------- */

/* As libbzip2's BZ2_hbCreateDecodeTables, then a table of the symbols whose codes are at most
 * FAST_BITS long, found as the slow way finds them. */
static void make_tables(bz_t* s, int t, int alpha) {
    cx_i32* limit = s->limit[t];
    cx_i32* base = s->base[t];
    cx_u16* perm = s->perm[t];
    const cx_u8* len = s->len[t];
    int min_len = 32, max_len = 0;
    for (int i = 0; i < alpha; i++) {
        if (len[i] > max_len) max_len = len[i];
        if (len[i] < min_len) min_len = len[i];
    }
    int pp = 0;
    for (int i = min_len; i <= max_len; i++) {
        for (int j = 0; j < alpha; j++) {
            if (len[j] == i) perm[pp++] = (cx_u16)j;
        }
    }
    for (int i = 0; i < CODE_LEN; i++) base[i] = 0;
    for (int i = 0; i < alpha; i++) base[len[i] + 1]++;
    for (int i = 1; i < CODE_LEN; i++) base[i] += base[i - 1];
    for (int i = 0; i < CODE_LEN; i++) limit[i] = 0;
    cx_i32 vec = 0;
    for (int i = min_len; i <= max_len; i++) {
        vec += base[i + 1] - base[i];
        limit[i] = vec - 1;
        vec <<= 1;
    }
    for (int i = min_len + 1; i <= max_len; i++) base[i] = ((limit[i - 1] + 1) << 1) - base[i];
    s->min_len[t] = min_len;

    cx_u16* fast = s->fast[t];
    for (cx_u32 w = 0; w < (1u << FAST_BITS); w++) {
        cx_u16 e = SLOW;
        int zn = min_len;
        if (zn <= FAST_BITS) {
            cx_i32 zvec = (cx_i32)(w >> (FAST_BITS - zn));
            for (;;) {
                if (zn > 20) break;
                if (zvec <= limit[zn]) {
                    cx_i32 at = zvec - base[zn];
                    if (at >= 0 && at < ALPHA && perm[at] != NO_SYMBOL) e = (cx_u16)(zn << 9 | perm[at]);
                    break;
                }
                zn++;
                if (zn > FAST_BITS) break;
                zvec = zvec << 1 | (cx_i32)(w >> (FAST_BITS - zn) & 1);
            }
        }
        fast[w] = e;
    }
}

/* The next symbol with table t, as libbzip2's GET_MTF_VAL reads it. */
#define SYMBOL(sym)                                                                     \
    do {                                                                                \
        if (s->nb < 20) refill(s, in, n);                                               \
        cx_u16 e_ = SLOW;                                                               \
        if (s->nb >= FAST_BITS) e_ = fast[(cx_u32)(s->bb >> (s->nb - FAST_BITS)) & ((1u << FAST_BITS) - 1)]; \
        if (e_ != SLOW) {                                                               \
            s->nb -= e_ >> 9;                                                           \
            (sym) = e_ & 0x1ff;                                                         \
        } else {                                                                        \
            int zn_ = min_len;                                                          \
            cx_u32 zvec_, zj_;                                                          \
            GET(zvec_, zn_);                                                            \
            for (;;) {                                                                  \
                if (zn_ > 20) return BZ_CORRUPT;                                        \
                if ((cx_i32)zvec_ <= limit[zn_]) break;                                 \
                zn_++;                                                                  \
                GET(zj_, 1);                                                            \
                zvec_ = zvec_ << 1 | zj_;                                               \
            }                                                                           \
            cx_i32 at_ = (cx_i32)zvec_ - base[zn_];                                     \
            if (at_ < 0 || at_ >= ALPHA || perm[at_] == NO_SYMBOL) return BZ_CORRUPT;   \
            (sym) = perm[at_];                                                          \
        }                                                                               \
    } while (0)

/* The next symbol of the block: a new group of 50 takes its table first. */
#define NEXT(sym)                                                \
    do {                                                         \
        if (group_pos == 0) {                                    \
            group_no++;                                          \
            if (group_no >= n_selectors) return BZ_CORRUPT;      \
            group_pos = 50;                                      \
            int g_ = s->selector[group_no];                      \
            min_len = s->min_len[g_];                            \
            limit = s->limit[g_];                                \
            base = s->base[g_];                                  \
            perm = s->perm[g_];                                  \
            fast = s->fast[g_];                                  \
        }                                                        \
        group_pos--;                                             \
        SYMBOL(sym);                                             \
    } while (0)

/* --- Blocks --------------------------------------------------------------------------------- */

/* Reads a block after its marker into tt and sets up its output, as libbzip2's BZ2_decompress
 * does from BZ_X_BCRC_1 to BZ_X_OUTPUT. */
static int read_block(bz_t* s, const cx_u8* in, cx_usize n) {
    cx_u32 v;
    GET(s->stored_crc, 32);
    GET(v, 1);
    s->randomised = (int)v;
    GET(v, 24);
    s->orig_ptr = (cx_i32)v;
    if (s->orig_ptr > 10 + 100000 * (cx_i32)s->level) return BZ_CORRUPT;

    cx_u32 used16;
    GET(used16, 16);
    int n_in_use = 0;
    for (int i = 0; i < 16; i++) {
        if (!(used16 >> (15 - i) & 1)) continue;
        cx_u32 used;
        GET(used, 16);
        for (int j = 0; j < 16; j++) {
            if (used >> (15 - j) & 1) s->seq_to_unseq[n_in_use++] = (cx_u8)(i * 16 + j);
        }
    }
    if (n_in_use == 0) return BZ_CORRUPT;
    int alpha = n_in_use + 2;

    cx_u32 groups, n_sel;
    GET(groups, 3);
    if (groups < 2 || groups > GROUPS) return BZ_CORRUPT;
    GET(n_sel, 15);
    if (n_sel < 1) return BZ_CORRUPT;
    cx_u8 order[GROUPS];
    for (int i = 0; i < GROUPS; i++) order[i] = (cx_u8)i;
    for (cx_u32 i = 0; i < n_sel; i++) {
        cx_u32 j = 0;
        for (;;) {
            GET(v, 1);
            if (v == 0) break;
            j++;
            if (j >= groups) return BZ_CORRUPT;
        }
        /* libbzip2 1.0.8 reads selectors past its 18002 and drops them. */
        if (i < MAX_SELECTORS) {
            cx_u8 g = order[j];
            for (; j > 0; j--) order[j] = order[j - 1];
            order[0] = g;
            s->selector[i] = g;
        }
    }
    int n_selectors = n_sel > MAX_SELECTORS ? MAX_SELECTORS : (int)n_sel;

    for (cx_u32 t = 0; t < groups; t++) {
        cx_u32 curr;
        GET(curr, 5);
        for (int i = 0; i < alpha; i++) {
            for (;;) {
                if (curr < 1 || curr > 20) return BZ_CORRUPT;
                GET(v, 1);
                if (v == 0) break;
                GET(v, 1);
                if (v == 0) curr++;
                else curr--;
            }
            s->len[t][i] = (cx_u8)curr;
        }
    }
    for (cx_u32 t = 0; t < groups; t++) make_tables(s, (int)t, alpha);

    /* The symbols. */
    cx_i32 eob = n_in_use + 1;
    cx_i32 most = 100000 * (cx_i32)s->level;
    cx_u32* tt = s->tt;
    int group_no = -1, group_pos = 0;
    int min_len = 0;
    const cx_i32 *limit = 0, *base = 0;
    const cx_u16 *perm = 0, *fast = 0;
    /* Two past the 256 a symbol an earlier block's perm left can reach, as libbzip2's can. */
    cx_u8 mtf[258];
    for (int i = 0; i < 256; i++) mtf[i] = (cx_u8)i;
    mtf[256] = mtf[257] = 0;
    for (int i = 0; i < 256; i++) s->unzftab[i] = 0;
    cx_i32 nblock = 0;
    cx_u32 sym;
    NEXT(sym);
    for (;;) {
        if ((cx_i32)sym == eob) break;
        if (sym <= 1) {
            cx_i32 es = -1, run = 1;
            do {
                if (run >= 2 * 1024 * 1024) return BZ_CORRUPT;
                es += (cx_i32)(sym + 1) * run;
                run *= 2;
                NEXT(sym);
            } while (sym <= 1);
            es++;
            cx_u8 uc = s->seq_to_unseq[mtf[0]];
            s->unzftab[uc] += es;
            if (es > most - nblock) return BZ_CORRUPT;
            for (cx_i32 k = 0; k < es; k++) tt[nblock + k] = uc;
            nblock += es;
            continue;
        }
        if (nblock >= most) return BZ_CORRUPT;
        cx_u32 nn = sym - 1;
        cx_u8 m = mtf[nn];
        memmove(mtf + 1, mtf, nn);
        mtf[0] = m;
        cx_u8 uc = s->seq_to_unseq[m];
        s->unzftab[uc]++;
        tt[nblock++] = uc;
        NEXT(sym);
    }

    if (s->orig_ptr < 0 || s->orig_ptr >= nblock) return BZ_CORRUPT;
    for (int i = 0; i < 256; i++) {
        if (s->unzftab[i] < 0 || s->unzftab[i] > nblock) return BZ_CORRUPT;
    }
    cx_i32 cftab[257];
    cftab[0] = 0;
    for (int i = 1; i <= 256; i++) cftab[i] = s->unzftab[i - 1];
    for (int i = 1; i <= 256; i++) cftab[i] += cftab[i - 1];
    for (int i = 0; i <= 256; i++) {
        if (cftab[i] < 0 || cftab[i] > nblock) return BZ_CORRUPT;
    }
    for (cx_i32 i = 0; i < nblock; i++) {
        cx_u8 uc = (cx_u8)(tt[i] & 0xff);
        tt[cftab[uc]] |= (cx_u32)i << 8;
        cftab[uc]++;
    }

    s->nblock = nblock;
    s->out_len = 0;
    s->out_ch = 0;
    s->block_crc = 0xffffffffu;
    s->tpos = tt[s->orig_ptr] >> 8;
    s->used = 0;
    s->r_to_go = 0;
    s->r_pos = 0;
    return 0;
}

/* The next byte of the transform's output into k, or BZ_CORRUPT. */
#define FETCH(k)                                             \
    do {                                                     \
        if (tpos >= limit_pos) return BZ_CORRUPT;            \
        tpos = tt[tpos];                                     \
        (k) = (int)(tpos & 0xff);                            \
        tpos >>= 8;                                          \
        if (rand) {                                          \
            if (s->r_to_go == 0) {                           \
                s->r_to_go = RNUMS[s->r_pos];                \
                if (++s->r_pos == 512) s->r_pos = 0;         \
            }                                                \
            s->r_to_go--;                                    \
            (k) ^= s->r_to_go == 1;                          \
        }                                                    \
        used++;                                              \
    } while (0)

/* Writes the block's text into out from *len up to cap, undoing the runs, as libbzip2's
 * unRLE_obuf_to_output_FAST does: BZ_END when it's all out, BZ_FULL when out is. */
static int write_block(bz_t* s, cx_u8* out, cx_usize* len, cx_usize cap) {
    const cx_u32* tt = s->tt;
    cx_u32 tpos = s->tpos;
    cx_u32 limit_pos = 100000u * s->level;
    cx_i32 used = s->used, end = s->nblock + 1;
    int rand = s->randomised;
    int k0 = s->k0, k1, out_len = s->out_len, out_ch = s->out_ch;
    cx_usize o = *len, from = o;
    int r = BZ_FULL;
    if (used == 0) FETCH(k0);
    for (;;) {
        if (out_len > 0) {
            cx_usize room = cap - o;
            if ((cx_usize)out_len > room) {
                memset(out + o, out_ch, room);
                o += room;
                out_len -= (int)room;
                break;
            }
            memset(out + o, out_ch, (cx_usize)out_len);
            o += (cx_usize)out_len;
            out_len = 0;
        }
        if (used == end) {
            r = BZ_END;
            break;
        }
        if (used > end) return BZ_CORRUPT;
        if (o == cap) break;
        out_len = 1;
        out_ch = k0;
        FETCH(k1);
        if (used == end) continue;
        if (k1 != k0) {
            k0 = k1;
            continue;
        }
        out_len = 2;
        FETCH(k1);
        if (used == end) continue;
        if (k1 != k0) {
            k0 = k1;
            continue;
        }
        out_len = 3;
        FETCH(k1);
        if (used == end) continue;
        if (k1 != k0) {
            k0 = k1;
            continue;
        }
        FETCH(k1);
        out_len = k1 + 4;
        FETCH(k0);
    }
    cx_u32 crc = s->block_crc;
    for (cx_usize i = from; i < o; i++) crc = crc << 8 ^ s->crc_table[(crc >> 24) ^ out[i]];
    s->block_crc = crc;
    s->tpos = tpos;
    s->used = used;
    s->k0 = k0;
    s->out_len = out_len;
    s->out_ch = out_ch;
    *len = o;
    return r;
}

/* --- Streams -------------------------------------------------------------------------------- */

/* Carries on with the stream cx_bz2_start started in in[0..n), writing what it decodes to into
 * out from *len up to cap, and moving *len on. BZ_END at its end, BZ_FULL when out is full, or an
 * error. */
cx_i32 cx_bz2_run(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize* len, cx_usize cap) {
    bz_t* s = (bz_t*)ctx;
    for (;;) {
        switch (s->phase) {
        case PHASE_HEADER: {
            static const cx_u8 magic[3] = {'B', 'Z', 'h'};
            cx_u32 v;
            for (int i = 0; i < 3; i++) {
                GET(v, 8);
                if (v != magic[i]) return BZ_FORMAT;
            }
            GET(v, 8);
            if (v < '1' || v > '9') return BZ_FORMAT;
            s->level = v - '0';
            cx_usize need = 100000u * s->level;
            if (s->tt_cap < need) {
                if (s->tt) cx_free(s->tt, s->tt_cap * sizeof(cx_u32));
                s->tt = (cx_u32*)cx_alloc(need * sizeof(cx_u32));
                s->tt_cap = need;
            }
            s->phase = PHASE_BLOCK;
            break;
        }
        case PHASE_BLOCK: {
            static const cx_u8 block[6] = {0x31, 0x41, 0x59, 0x26, 0x53, 0x59};
            static const cx_u8 last[6] = {0x17, 0x72, 0x45, 0x38, 0x50, 0x90};
            cx_u32 v;
            GET(v, 8);
            const cx_u8* want = v == 0x17 ? last : block;
            if (v != want[0]) return BZ_CORRUPT;
            for (int i = 1; i < 6; i++) {
                GET(v, 8);
                if (v != want[i]) return BZ_CORRUPT;
            }
            if (want == last) {
                GET(v, 32);
                if (v != s->combined_crc) return BZ_CHECKSUM;
                s->phase = PHASE_DONE;
                return BZ_END;
            }
            int r = read_block(s, in, n);
            if (r != 0) return r;
            s->phase = PHASE_OUTPUT;
            break;
        }
        case PHASE_OUTPUT: {
            int r = write_block(s, out, len, cap);
            if (r != BZ_END) return r;
            cx_u32 crc = ~s->block_crc;
            if (crc != s->stored_crc) return BZ_CHECKSUM;
            s->combined_crc = (s->combined_crc << 1 | s->combined_crc >> 31) ^ crc;
            s->phase = PHASE_BLOCK;
            break;
        }
        default:
            return BZ_END;
        }
    }
}
