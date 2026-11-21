/* bzip2 compression, behind std.bz2's `compress` (CLX-400). Linked only into programs that
 * compress.
 *
 * It writes the bytes Python's bz2.compress writes, which is libbzip2 1.0.8's BZ2_bzCompress, so
 * it makes the same choices libbzip2 does, step by step: runs of 4 to 255 bytes become 4 and a
 * count, as bzlib.c's ADD_CHAR_TO_BLOCK makes them, into blocks of 100000 * level - 19 bytes or
 * just over; each block's rotations are sorted by the fallback sort of blocksort.c, which sorts
 * them as its main sort does, so the transform is the same; then compress.c's move-to-front and
 * its choice of how many Huffman tables, their first lengths, four rounds of picking a table for
 * each 50 symbols and remaking them, and the lengths of huffman.c, capped at 17 bits.
 *
 * Sorting rotations can only order identical ones, in a block that's one piece repeated, as the
 * sort they're sorted by does; for such a block of 10000 bytes or more libbzip2's main sort runs
 * out of its budget and falls back to the same sort, so that's the same too.
 *
 * Where a block ends depends only on the bytes before it, so the first pass cuts the input into
 * blocks one after another, and up to 8 full blocks are then sorted and coded at once on
 * `parallel for`'s threads, each into bits of its own, which are put one after another. The
 * stream is the same however many threads made it.
 *
 * The fallback sort, the table lengths and the choice of tables are adapted from libbzip2 1.0.8,
 * whose notice follows:
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

#include <stdint.h>
#include <string.h>

#define GROUPS 6
#define ALPHA 258
#define G_SIZE 50
#define ITERS 4
#define MAX_SELECTORS 18002
#define LESSER_ICOST 0
#define GREATER_ICOST 15

typedef struct {
    cx_u8* p;
    cx_usize len, cap;
    cx_u64 acc;
    int live;
} out_t;

/* A block as the first pass leaves it, and then what it compresses to: whole bytes, and the bits
 * after them in the low `live` bits of o.acc, since blocks don't end on a byte. */
typedef struct {
    cx_u8* block;
    cx_i32 nblock;
    cx_u32 crc;
    cx_u8 in_use[256];
    out_t o;
} blk_t;

/* What compressing one block takes, on whichever thread does it. */
typedef struct {
    const cx_u8* block;
    cx_i32 nblock;
    const cx_u8* in_use;
    out_t* o;
    cx_u32* ptr;
    cx_u32* eclass;
    cx_u32* bhtab;
    cx_usize bhtab_words;
    cx_u16* mtfv;
    cx_i32 nmtf;
    cx_u8 unseq_to_seq[256];
    cx_i32 n_in_use;
    cx_i32 orig_ptr;
    cx_i32 mtf_freq[ALPHA];
    cx_u8 len[GROUPS][ALPHA];
    cx_i32 code[GROUPS][ALPHA];
    cx_i32 rfreq[GROUPS][ALPHA];
    cx_u8 selector[MAX_SELECTORS];
    cx_u8 selector_mtf[MAX_SELECTORS];
} coder_t;

/* --- Output --------------------------------------------------------------------------------- */

static void out_grow(out_t* o, cx_usize more) {
    if (o->len + more <= o->cap) return;
    cx_usize cap = o->cap * 2;
    if (cap < o->len + more) cap = o->len + more;
    if (cap < 4096) cap = 4096;
    o->p = (cx_u8*)cx_realloc(o->p, o->cap, cap);
    o->cap = cap;
}

/* n bits of v, MSB first, n at most 32. */
static inline void put_bits(out_t* o, int n, cx_u32 v) {
    o->acc = o->acc << n | v;
    o->live += n;
    while (o->live >= 8) {
        o->live -= 8;
        o->p[o->len++] = (cx_u8)(o->acc >> o->live);
    }
}

static void put_byte(out_t* o, cx_u32 b) { put_bits(o, 8, b); }

static void put_u32(out_t* o, cx_u32 v) {
    put_bits(o, 16, v >> 16);
    put_bits(o, 16, v & 0xffff);
}

/* --- The fallback sort, as blocksort.c's -------------------------------------------------- */

static void fallback_simple_sort(cx_u32* fmap, const cx_u32* eclass, cx_i32 lo, cx_i32 hi) {
    cx_i32 i, j;
    if (lo == hi) return;
    if (hi - lo > 3) {
        for (i = hi - 4; i >= lo; i--) {
            cx_u32 tmp = fmap[i];
            cx_u32 ec = eclass[tmp];
            for (j = i + 4; j <= hi && ec > eclass[fmap[j]]; j += 4) fmap[j - 4] = fmap[j];
            fmap[j - 4] = tmp;
        }
    }
    for (i = hi - 1; i >= lo; i--) {
        cx_u32 tmp = fmap[i];
        cx_u32 ec = eclass[tmp];
        for (j = i + 1; j <= hi && ec > eclass[fmap[j]]; j++) fmap[j - 1] = fmap[j];
        fmap[j - 1] = tmp;
    }
}

#define FSWAP(a, b)          \
    do {                     \
        cx_u32 t_ = (a);     \
        (a) = (b);           \
        (b) = t_;            \
    } while (0)

static void fvswap(cx_u32* fmap, cx_i32 p1, cx_i32 p2, cx_i32 n) {
    while (n > 0) {
        FSWAP(fmap[p1], fmap[p2]);
        p1++, p2++, n--;
    }
}

#define FQ_SMALL 10
#define FQ_STACK 100

static void fallback_qsort3(cx_u32* fmap, const cx_u32* eclass, cx_i32 lo_st, cx_i32 hi_st) {
    cx_i32 stack_lo[FQ_STACK], stack_hi[FQ_STACK];
    cx_i32 sp = 0;
    cx_u32 r = 0;
    stack_lo[sp] = lo_st, stack_hi[sp] = hi_st, sp++;
    while (sp > 0) {
        sp--;
        cx_i32 lo = stack_lo[sp], hi = stack_hi[sp];
        if (hi - lo < FQ_SMALL) {
            fallback_simple_sort(fmap, eclass, lo, hi);
            continue;
        }
        r = (r * 7621 + 1) % 32768;
        cx_u32 r3 = r % 3, med;
        if (r3 == 0) med = eclass[fmap[lo]];
        else if (r3 == 1) med = eclass[fmap[(lo + hi) >> 1]];
        else med = eclass[fmap[hi]];
        cx_i32 un_lo = lo, lt_lo = lo, un_hi = hi, gt_hi = hi, n, m;
        for (;;) {
            for (;;) {
                if (un_lo > un_hi) break;
                n = (cx_i32)eclass[fmap[un_lo]] - (cx_i32)med;
                if (n == 0) {
                    FSWAP(fmap[un_lo], fmap[lt_lo]);
                    lt_lo++, un_lo++;
                    continue;
                }
                if (n > 0) break;
                un_lo++;
            }
            for (;;) {
                if (un_lo > un_hi) break;
                n = (cx_i32)eclass[fmap[un_hi]] - (cx_i32)med;
                if (n == 0) {
                    FSWAP(fmap[un_hi], fmap[gt_hi]);
                    gt_hi--, un_hi--;
                    continue;
                }
                if (n < 0) break;
                un_hi--;
            }
            if (un_lo > un_hi) break;
            FSWAP(fmap[un_lo], fmap[un_hi]);
            un_lo++, un_hi--;
        }
        if (gt_hi < lt_lo) continue;
        n = lt_lo - lo < un_lo - lt_lo ? lt_lo - lo : un_lo - lt_lo;
        fvswap(fmap, lo, un_lo - n, n);
        m = hi - gt_hi < gt_hi - un_hi ? hi - gt_hi : gt_hi - un_hi;
        fvswap(fmap, un_lo, hi - m + 1, m);
        n = lo + un_lo - lt_lo - 1;
        m = hi - (gt_hi - un_hi) + 1;
        if (n - lo > hi - m) {
            stack_lo[sp] = lo, stack_hi[sp] = n, sp++;
            stack_lo[sp] = m, stack_hi[sp] = hi, sp++;
        } else {
            stack_lo[sp] = m, stack_hi[sp] = hi, sp++;
            stack_lo[sp] = lo, stack_hi[sp] = n, sp++;
        }
    }
}

#define SET_BH(z) bhtab[(z) >> 5] |= (cx_u32)1 << ((z) & 31)
#define CLEAR_BH(z) bhtab[(z) >> 5] &= ~((cx_u32)1 << ((z) & 31))
#define ISSET_BH(z) (bhtab[(z) >> 5] & ((cx_u32)1 << ((z) & 31)))
#define WORD_BH(z) bhtab[(z) >> 5]
#define UNALIGNED_BH(z) ((z) & 0x1f)

/* Sorts the block's rotations into fmap, as blocksort.c's fallbackSort. */
static void fallback_sort(cx_u32* fmap, cx_u32* eclass, cx_u32* bhtab, cx_usize bh_words, const cx_u8* block, cx_i32 nblock) {
    cx_i32 ftab[257];
    cx_i32 i, j, k, l, r, h;
    for (i = 0; i < 257; i++) ftab[i] = 0;
    for (i = 0; i < nblock; i++) ftab[block[i]]++;
    for (i = 1; i < 257; i++) ftab[i] += ftab[i - 1];
    for (i = 0; i < nblock; i++) {
        j = block[i];
        k = ftab[j] - 1;
        ftab[j] = k;
        fmap[k] = (cx_u32)i;
    }
    memset(bhtab, 0, bh_words * sizeof(cx_u32));
    for (i = 0; i < 256; i++) SET_BH(ftab[i]);
    for (i = 0; i < 32; i++) {
        SET_BH(nblock + 2 * i);
        CLEAR_BH(nblock + 2 * i + 1);
    }
    h = 1;
    for (;;) {
        j = 0;
        for (i = 0; i < nblock; i++) {
            if (ISSET_BH(i)) j = i;
            k = (cx_i32)fmap[i] - h;
            if (k < 0) k += nblock;
            eclass[k] = (cx_u32)j;
        }
        cx_i32 not_done = 0;
        r = -1;
        for (;;) {
            k = r + 1;
            while (ISSET_BH(k) && UNALIGNED_BH(k)) k++;
            if (ISSET_BH(k)) {
                while (WORD_BH(k) == 0xffffffffu) k += 32;
                while (ISSET_BH(k)) k++;
            }
            l = k - 1;
            if (l >= nblock) break;
            while (!ISSET_BH(k) && UNALIGNED_BH(k)) k++;
            if (!ISSET_BH(k)) {
                while (WORD_BH(k) == 0) k += 32;
                while (!ISSET_BH(k)) k++;
            }
            r = k - 1;
            if (r >= nblock) break;
            if (r > l) {
                not_done += r - l + 1;
                fallback_qsort3(fmap, eclass, l, r);
                cx_i32 cc = -1;
                for (i = l; i <= r; i++) {
                    cx_i32 cc1 = (cx_i32)eclass[fmap[i]];
                    if (cc != cc1) {
                        SET_BH(i);
                        cc = cc1;
                    }
                }
            }
        }
        h *= 2;
        if (h > nblock || not_done == 0) break;
    }
}

/* --- Huffman code lengths, as huffman.c's ---------------------------------------------------- */

#define WEIGHTOF(w) ((w) & 0xffffff00)
#define DEPTHOF(w) ((w) & 0x000000ff)

static void make_code_lengths(cx_u8* len, const cx_i32* freq, cx_i32 alpha, cx_i32 max_len) {
    cx_i32 heap[ALPHA + 2];
    cx_i32 weight[ALPHA * 2];
    cx_i32 parent[ALPHA * 2];
    for (cx_i32 i = 0; i < alpha; i++) weight[i + 1] = (freq[i] == 0 ? 1 : freq[i]) << 8;
    for (;;) {
        cx_i32 nodes = alpha, nheap = 0;
        heap[0] = 0;
        weight[0] = 0;
        parent[0] = -2;
        for (cx_i32 i = 1; i <= alpha; i++) {
            parent[i] = -1;
            nheap++;
            heap[nheap] = i;
            cx_i32 zz = nheap, tmp = heap[zz];
            while (weight[tmp] < weight[heap[zz >> 1]]) {
                heap[zz] = heap[zz >> 1];
                zz >>= 1;
            }
            heap[zz] = tmp;
        }
        while (nheap > 1) {
            cx_i32 pick[2];
            for (int p = 0; p < 2; p++) {
                pick[p] = heap[1];
                heap[1] = heap[nheap];
                nheap--;
                cx_i32 zz = 1, tmp = heap[zz];
                for (;;) {
                    cx_i32 yy = zz << 1;
                    if (yy > nheap) break;
                    if (yy < nheap && weight[heap[yy + 1]] < weight[heap[yy]]) yy++;
                    if (weight[tmp] < weight[heap[yy]]) break;
                    heap[zz] = heap[yy];
                    zz = yy;
                }
                heap[zz] = tmp;
            }
            nodes++;
            parent[pick[0]] = parent[pick[1]] = nodes;
            cx_i32 d0 = DEPTHOF(weight[pick[0]]), d1 = DEPTHOF(weight[pick[1]]);
            weight[nodes] = (WEIGHTOF(weight[pick[0]]) + WEIGHTOF(weight[pick[1]])) | (1 + (d0 > d1 ? d0 : d1));
            parent[nodes] = -1;
            nheap++;
            heap[nheap] = nodes;
            cx_i32 zz = nheap, tmp = heap[zz];
            while (weight[tmp] < weight[heap[zz >> 1]]) {
                heap[zz] = heap[zz >> 1];
                zz >>= 1;
            }
            heap[zz] = tmp;
        }
        int too_long = 0;
        for (cx_i32 i = 1; i <= alpha; i++) {
            cx_i32 j = 0, k = i;
            while (parent[k] >= 0) {
                k = parent[k];
                j++;
            }
            len[i - 1] = (cx_u8)j;
            if (j > max_len) too_long = 1;
        }
        if (!too_long) break;
        for (cx_i32 i = 1; i <= alpha; i++) {
            cx_i32 j = weight[i] >> 8;
            j = 1 + j / 2;
            weight[i] = j << 8;
        }
    }
}

/* --- Blocks, as compress.c writes them ------------------------------------------------------ */

static void make_mtf_values(coder_t* s) {
    cx_u8 yy[256];
    s->n_in_use = 0;
    for (int i = 0; i < 256; i++) {
        if (s->in_use[i]) s->unseq_to_seq[i] = (cx_u8)s->n_in_use++;
    }
    cx_i32 eob = s->n_in_use + 1;
    for (cx_i32 i = 0; i <= eob; i++) s->mtf_freq[i] = 0;
    cx_i32 wr = 0, zpend = 0;
    for (cx_i32 i = 0; i < s->n_in_use; i++) yy[i] = (cx_u8)i;
    cx_u16* mtfv = s->mtfv;
    for (cx_i32 i = 0; i < s->nblock; i++) {
        cx_i32 j = (cx_i32)s->ptr[i] - 1;
        if (j < 0) j += s->nblock;
        cx_u8 ll = s->unseq_to_seq[s->block[j]];
        if (yy[0] == ll) {
            zpend++;
            continue;
        }
        if (zpend > 0) {
            zpend--;
            for (;;) {
                cx_u16 sym = zpend & 1 ? 1 : 0;
                mtfv[wr++] = sym;
                s->mtf_freq[sym]++;
                if (zpend < 2) break;
                zpend = (zpend - 2) / 2;
            }
            zpend = 0;
        }
        cx_u8 tmp = yy[1];
        yy[1] = yy[0];
        cx_i32 at = 1;
        while (ll != tmp) {
            at++;
            cx_u8 tmp2 = tmp;
            tmp = yy[at];
            yy[at] = tmp2;
        }
        yy[0] = tmp;
        mtfv[wr++] = (cx_u16)(at + 1);
        s->mtf_freq[at + 1]++;
    }
    if (zpend > 0) {
        zpend--;
        for (;;) {
            cx_u16 sym = zpend & 1 ? 1 : 0;
            mtfv[wr++] = sym;
            s->mtf_freq[sym]++;
            if (zpend < 2) break;
            zpend = (zpend - 2) / 2;
        }
    }
    mtfv[wr++] = (cx_u16)eob;
    s->mtf_freq[eob]++;
    s->nmtf = wr;
}

static void send_mtf_values(coder_t* s) {
    cx_i32 alpha = s->n_in_use + 2;
    const cx_u16* mtfv = s->mtfv;
    out_t* o = s->o;
    for (int t = 0; t < GROUPS; t++) {
        for (cx_i32 v = 0; v < alpha; v++) s->len[t][v] = GREATER_ICOST;
    }
    int groups = s->nmtf < 200 ? 2 : s->nmtf < 600 ? 3 : s->nmtf < 1200 ? 4 : s->nmtf < 2400 ? 5 : 6;

    /* The first tables: each covers a run of symbols with about its share of the counts. */
    {
        cx_i32 parts = groups, rem = s->nmtf, gs = 0;
        while (parts > 0) {
            cx_i32 target = rem / parts, ge = gs - 1, got = 0;
            while (got < target && ge < alpha - 1) {
                ge++;
                got += s->mtf_freq[ge];
            }
            if (ge > gs && parts != groups && parts != 1 && (groups - parts) % 2 == 1) {
                got -= s->mtf_freq[ge];
                ge--;
            }
            for (cx_i32 v = 0; v < alpha; v++) s->len[parts - 1][v] = v >= gs && v <= ge ? LESSER_ICOST : GREATER_ICOST;
            parts--;
            gs = ge + 1;
            rem -= got;
        }
    }

    cx_i32 n_selectors = 0;
    for (int iter = 0; iter < ITERS; iter++) {
        for (int t = 0; t < groups; t++) {
            for (cx_i32 v = 0; v < alpha; v++) s->rfreq[t][v] = 0;
        }
        n_selectors = 0;
        for (cx_i32 gs = 0; gs < s->nmtf;) {
            cx_i32 ge = gs + G_SIZE - 1;
            if (ge >= s->nmtf) ge = s->nmtf - 1;
            cx_u16 cost[GROUPS] = {0};
            for (cx_i32 i = gs; i <= ge; i++) {
                cx_u16 icv = mtfv[i];
                for (int t = 0; t < groups; t++) cost[t] = (cx_u16)(cost[t] + s->len[t][icv]);
            }
            cx_i32 bc = 999999999, bt = -1;
            for (int t = 0; t < groups; t++) {
                if (cost[t] < bc) {
                    bc = cost[t];
                    bt = t;
                }
            }
            s->selector[n_selectors++] = (cx_u8)bt;
            for (cx_i32 i = gs; i <= ge; i++) s->rfreq[bt][mtfv[i]]++;
            gs = ge + 1;
        }
        for (int t = 0; t < groups; t++) make_code_lengths(s->len[t], s->rfreq[t], alpha, 17);
    }

    /* The selectors, move-to-front coded. */
    {
        cx_u8 pos[GROUPS];
        for (int i = 0; i < groups; i++) pos[i] = (cx_u8)i;
        for (cx_i32 i = 0; i < n_selectors; i++) {
            cx_u8 want = s->selector[i], tmp = pos[0];
            int j = 0;
            while (want != tmp) {
                j++;
                cx_u8 tmp2 = tmp;
                tmp = pos[j];
                pos[j] = tmp2;
            }
            pos[0] = tmp;
            s->selector_mtf[i] = (cx_u8)j;
        }
    }

    /* Codes for the lengths, as huffman.c's BZ2_hbAssignCodes. */
    for (int t = 0; t < groups; t++) {
        int min_len = 32, max_len = 0;
        for (cx_i32 i = 0; i < alpha; i++) {
            if (s->len[t][i] > max_len) max_len = s->len[t][i];
            if (s->len[t][i] < min_len) min_len = s->len[t][i];
        }
        cx_i32 vec = 0;
        for (int n = min_len; n <= max_len; n++) {
            for (cx_i32 i = 0; i < alpha; i++) {
                if (s->len[t][i] == n) s->code[t][i] = vec++;
            }
            vec <<= 1;
        }
    }

    /* Room for the rest of the block: at most 17 bits a symbol, and the tables. */
    out_grow(o, (cx_usize)s->nmtf * 17 / 8 + (cx_usize)n_selectors + 6 * ALPHA * 5 + 64);

    /* Which bytes the block uses. */
    {
        int used16[16];
        for (int i = 0; i < 16; i++) {
            used16[i] = 0;
            for (int j = 0; j < 16; j++) {
                if (s->in_use[i * 16 + j]) used16[i] = 1;
            }
        }
        for (int i = 0; i < 16; i++) put_bits(o, 1, (cx_u32)used16[i]);
        for (int i = 0; i < 16; i++) {
            if (!used16[i]) continue;
            for (int j = 0; j < 16; j++) put_bits(o, 1, s->in_use[i * 16 + j] ? 1u : 0u);
        }
    }

    put_bits(o, 3, (cx_u32)groups);
    put_bits(o, 15, (cx_u32)n_selectors);
    for (cx_i32 i = 0; i < n_selectors; i++) {
        for (int j = 0; j < s->selector_mtf[i]; j++) put_bits(o, 1, 1);
        put_bits(o, 1, 0);
    }

    for (int t = 0; t < groups; t++) {
        cx_i32 curr = s->len[t][0];
        put_bits(o, 5, (cx_u32)curr);
        for (cx_i32 i = 0; i < alpha; i++) {
            while (curr < s->len[t][i]) {
                put_bits(o, 2, 2);
                curr++;
            }
            while (curr > s->len[t][i]) {
                put_bits(o, 2, 3);
                curr--;
            }
            put_bits(o, 1, 0);
        }
    }

    cx_i32 sel = 0;
    for (cx_i32 gs = 0; gs < s->nmtf;) {
        cx_i32 ge = gs + G_SIZE - 1;
        if (ge >= s->nmtf) ge = s->nmtf - 1;
        const cx_u8* len = s->len[s->selector[sel]];
        const cx_i32* code = s->code[s->selector[sel]];
        for (cx_i32 i = gs; i <= ge; i++) put_bits(o, len[mtfv[i]], (cx_u32)code[mtfv[i]]);
        gs = ge + 1;
        sel++;
    }
}

/* Compresses a block the first pass made into its own bits, as compress.c's BZ2_compressBlock
 * writes one: its marker, its CRC, a 0 for not randomised, where the text is in the sorted
 * rotations, and the rest. */
static void code_block(blk_t* b) {
    coder_t* s = (coder_t*)cx_alloc(sizeof(coder_t));
    memset(s, 0, sizeof *s);
    cx_usize n = (cx_usize)b->nblock;
    s->block = b->block;
    s->nblock = b->nblock;
    s->in_use = b->in_use;
    s->o = &b->o;
    s->ptr = (cx_u32*)cx_alloc(n * sizeof(cx_u32));
    s->eclass = (cx_u32*)cx_alloc(n * sizeof(cx_u32));
    s->bhtab_words = n / 32 + 8;
    s->bhtab = (cx_u32*)cx_alloc(s->bhtab_words * sizeof(cx_u32));
    s->mtfv = (cx_u16*)cx_alloc((n + 8) * sizeof(cx_u16));

    fallback_sort(s->ptr, s->eclass, s->bhtab, s->bhtab_words, s->block, s->nblock);
    s->orig_ptr = -1;
    for (cx_i32 i = 0; i < s->nblock; i++) {
        if (s->ptr[i] == 0) {
            s->orig_ptr = i;
            break;
        }
    }
    out_t* o = s->o;
    o->len = 0;
    o->acc = 0;
    o->live = 0;
    out_grow(o, n / 2 + 64);
    put_byte(o, 0x31);
    put_byte(o, 0x41);
    put_byte(o, 0x59);
    put_byte(o, 0x26);
    put_byte(o, 0x53);
    put_byte(o, 0x59);
    put_u32(o, b->crc);
    put_bits(o, 1, 0);
    put_bits(o, 24, (cx_u32)s->orig_ptr);
    make_mtf_values(s);
    send_mtf_values(s);

    cx_free(s->ptr, n * sizeof(cx_u32));
    cx_free(s->eclass, n * sizeof(cx_u32));
    cx_free(s->bhtab, s->bhtab_words * sizeof(cx_u32));
    cx_free(s->mtfv, (n + 8) * sizeof(cx_u16));
    cx_free(s, sizeof(coder_t));
}

/* Puts src's bits after dst's. */
static void append_bits(out_t* dst, const out_t* src) {
    out_grow(dst, src->len + 16);
    if (dst->live == 0) {
        memcpy(dst->p + dst->len, src->p, src->len);
        dst->len += src->len;
    } else {
        for (cx_usize i = 0; i < src->len; i++) put_bits(dst, 8, src->p[i]);
    }
    if (src->live > 0) put_bits(dst, src->live, (cx_u32)(src->acc & ((1u << src->live) - 1)));
}

/* Blocks compressed at once: at most one for each thread `parallel for` has, and at most 8, as
 * each takes about 11 bytes for each of its own while it's sorted. */
#define MOST_AT_ONCE 8

typedef struct {
    cx_u32 crc_table[256];
    cx_i32 level, nblock_max;
    cx_usize block_cap;
    /* The blocks being filled and compressed, and how many are full. */
    blk_t blocks[MOST_AT_ONCE];
    int at_once, full;
    cx_u32 combined_crc;
    /* The run being read: its byte, 256 for none, and how long it is. */
    cx_u32 in_ch;
    cx_i32 in_len;
    out_t o;
} job_t;

static void code_blocks(void* ctx, cx_u64 begin, cx_u64 end) {
    job_t* j = (job_t*)ctx;
    for (cx_u64 k = begin; k < end; k++) code_block(&j->blocks[k]);
}

/* Compresses the full blocks, on all the processors when there are several, and puts them after
 * what's been written, in order. */
static void flush_blocks(job_t* j) {
    if (j->full == 0) return;
    if (j->full == 1) code_block(&j->blocks[0]);
    else cx_parallel((cx_u64)j->full, code_blocks, j);
    for (int k = 0; k < j->full; k++) append_bits(&j->o, &j->blocks[k].o);
    j->full = 0;
}

/* The block being filled. */
static blk_t* filling(job_t* j) { return &j->blocks[j->full]; }

static void start_block(job_t* j) {
    blk_t* b = filling(j);
    if (!b->block) b->block = (cx_u8*)cx_alloc(j->block_cap);
    b->nblock = 0;
    b->crc = 0xffffffffu;
    memset(b->in_use, 0, sizeof b->in_use);
}

/* Ends the block being filled, as BZ2_compressBlock starts, and starts the next. */
static void end_block(job_t* j) {
    blk_t* b = filling(j);
    b->crc = ~b->crc;
    j->combined_crc = (j->combined_crc << 1 | j->combined_crc >> 31) ^ b->crc;
    j->full++;
    if (j->full == j->at_once) flush_blocks(j);
    start_block(j);
}

/* The run read so far into the block, as bzlib.c's add_pair_to_block. */
static void add_run(job_t* j) {
    blk_t* blk = filling(j);
    cx_u8 ch = (cx_u8)j->in_ch;
    for (cx_i32 i = 0; i < j->in_len; i++) blk->crc = blk->crc << 8 ^ j->crc_table[(blk->crc >> 24) ^ ch];
    blk->in_use[ch] = 1;
    cx_u8* b = blk->block + blk->nblock;
    switch (j->in_len) {
    case 1: b[0] = ch; blk->nblock += 1; break;
    case 2: b[0] = b[1] = ch; blk->nblock += 2; break;
    case 3: b[0] = b[1] = b[2] = ch; blk->nblock += 3; break;
    default:
        blk->in_use[j->in_len - 4] = 1;
        b[0] = b[1] = b[2] = b[3] = ch;
        b[4] = (cx_u8)(j->in_len - 4);
        blk->nblock += 5;
        break;
    }
}

/* Compresses in[0..n) at `level`, 1 to 9, into one stream as Python's bz2.compress writes it.
 * Gives a handle to it: cx_bz2e_size gives its size, and cx_bz2e_take copies it out and frees it. */
cx_u64 cx_bz2e_compress(const cx_u8* in, cx_u64 n, cx_u32 level) {
    job_t* j = (job_t*)cx_alloc(sizeof(job_t));
    memset(j, 0, sizeof *j);
    for (cx_u32 i = 0; i < 256; i++) {
        cx_u32 c = i << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
        j->crc_table[i] = c;
    }
    j->level = (cx_i32)level;
    j->nblock_max = 100000 * j->level - 19;
    /* A block is never more than 4 past nblock_max; no more than the input needs, either. */
    cx_u64 most = (cx_u64)j->nblock_max + 5;
    cx_u64 need = n + n / 4 + 5;
    j->block_cap = (cx_usize)(need < most ? need : most);
    cx_u64 blocks = n / (cx_u64)j->nblock_max + 1;
    cx_usize workers = cx_workers();
    j->at_once = (int)(workers < MOST_AT_ONCE ? workers : MOST_AT_ONCE);
    if ((cx_u64)j->at_once > blocks) j->at_once = (int)blocks;
    if (j->at_once < 1) j->at_once = 1;
    out_grow(&j->o, (cx_usize)(n / 3) + 64);
    put_byte(&j->o, 'B');
    put_byte(&j->o, 'Z');
    put_byte(&j->o, 'h');
    put_byte(&j->o, (cx_u32)('0' + j->level));

    j->in_ch = 256;
    j->in_len = 0;
    start_block(j);
    blk_t* b = filling(j);
    for (cx_u64 i = 0; i < n; i++) {
        cx_u32 c = in[i];
        if (c != j->in_ch && j->in_len == 1) {
            cx_u8 ch = (cx_u8)j->in_ch;
            b->crc = b->crc << 8 ^ j->crc_table[(b->crc >> 24) ^ ch];
            b->in_use[ch] = 1;
            b->block[b->nblock++] = ch;
            j->in_ch = c;
        } else if (c != j->in_ch || j->in_len == 255) {
            if (j->in_ch < 256) add_run(j);
            j->in_ch = c;
            j->in_len = 1;
        } else {
            j->in_len++;
        }
        /* libbzip2 compresses a block as soon as it's full, and the run being read goes on into
         * the next. */
        if (b->nblock >= j->nblock_max) {
            end_block(j);
            b = filling(j);
        }
    }
    if (j->in_ch < 256) add_run(j);
    if (b->nblock > 0) {
        b->crc = ~b->crc;
        j->combined_crc = (j->combined_crc << 1 | j->combined_crc >> 31) ^ b->crc;
        j->full++;
    }
    flush_blocks(j);

    out_t* o = &j->o;
    out_grow(o, 16);
    put_byte(o, 0x17);
    put_byte(o, 0x72);
    put_byte(o, 0x45);
    put_byte(o, 0x38);
    put_byte(o, 0x50);
    put_byte(o, 0x90);
    put_u32(o, j->combined_crc);
    if (o->live > 0) put_bits(o, 8 - o->live, 0);

    for (int k = 0; k < MOST_AT_ONCE; k++) {
        if (j->blocks[k].block) cx_free(j->blocks[k].block, j->block_cap);
        if (j->blocks[k].o.p) cx_free(j->blocks[k].o.p, j->blocks[k].o.cap);
    }
    return (cx_u64)(uintptr_t)j;
}

cx_u64 cx_bz2e_size(cx_u64 h) { return ((job_t*)(uintptr_t)h)->o.len; }

void cx_bz2e_take(cx_u64 h, cx_u8* dst) {
    job_t* j = (job_t*)(uintptr_t)h;
    if (j->o.len) memcpy(dst, j->o.p, j->o.len);
    if (j->o.p) cx_free(j->o.p, j->o.cap);
    cx_free(j, sizeof(job_t));
}
