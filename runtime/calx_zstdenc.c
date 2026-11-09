/* Zstandard compression, behind std.zstd's `compress` (CLX-183). Linked only into programs that
 * compress.
 *
 * Written from RFC 8878, with no libzstd code but the levels' settings below. A frame's text is
 * cut into blocks of 128 KiB, and each block's matches are found against the window before it: by
 * one hash table at the fastest levels, two at the next, rows of a hash table searched greedily or
 * lazily in the middle, and binary trees with a parser that prices every way through at the top.
 * A block whose text changes partway is split where that pays. Then they're written as the RFC
 * lays them out: the literals Huffman-coded and the sequences' codes FSE-coded, each with the
 * tables the block's own counts give, the ones the block before left, or the predefined ones,
 * whichever comes out smallest. A block that doesn't get smaller is stored as it is.
 *
 * Big inputs are cut into jobs of a size that depends only on the level, and the jobs compress at
 * once on `parallel for`'s threads: each matches into the text before it, and starts with tables
 * and repeated offsets of its own, so the frame is the same however many threads made it.
 *
 * Each level's settings, in LEVELS below, are those of libzstd's tables of levels, for inputs over
 * 256 KiB and for those up to 256, 128 and 16 KiB (lib/compress/clevels.h in zstd 1.5.7), so that
 * a level means what it means to `zstd`: the same window, so the same memory to decompress, and
 * searches about as deep. For big inputs, levels 13 and 14 take matches of 4 bytes rather than 5,
 * which on our files was smaller and faster with our trees. The tables' notice follows:
 *
 * BSD License
 *
 * For Zstandard software
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 *  * Neither the name Facebook, nor Meta, nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "calx_rt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(ZSE_CHECK) || defined(ZSE_TUNE)
#include <stdio.h>
#include <stdlib.h>
#endif
#ifdef ZSE_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif
#ifdef ZSE_CYCLES
#include <x86intrin.h>
static cx_u64 zse_cycles[8];
#endif
#if defined(__SSE2__)
#include <immintrin.h>
#endif

#define BLOCK_MAX ((cx_u32)1 << 17)
/* Room to put a block together in: its most, and the tables' descriptions. */
#define BLOCK_ROOM (BLOCK_MAX + 1024)
#define LL_MAX 35
#define ML_MAX 52
#define OF_MAX 31
#define LL_LOG 9
#define ML_LOG 9
#define OF_LOG 8
#define HUF_LOG 11
#define MAGIC 0xFD2FB528u
/* Literals fewer than this go raw: a tree costs more than it saves. */
#define HUF_MIN 32
/* How many bytes a hash or a match's first check reads past where it starts. */
#define READ 8

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

static inline void st64(cx_u8* p, cx_u64 v) { memcpy(p, &v, 8); }
static inline void st32(cx_u8* p, cx_u32 v) { memcpy(p, &v, 4); }
static inline void st16(cx_u8* p, cx_u32 v) {
    p[0] = (cx_u8)v;
    p[1] = (cx_u8)(v >> 8);
}
static inline void st24(cx_u8* p, cx_u32 v) {
    p[0] = (cx_u8)v;
    p[1] = (cx_u8)(v >> 8);
    p[2] = (cx_u8)(v >> 16);
}

/* The highest set bit of v, which isn't 0. */
static inline cx_u32 high_bit(cx_u32 v) { return 31 - (cx_u32)__builtin_clz(v); }

/* How far the bytes from p on agree with those from m on, which is before p, up to end. */
static inline cx_u32 count(const cx_u8* p, const cx_u8* m, const cx_u8* end) {
    const cx_u8* const start = p;
    while (p + 8 <= end) {
        const cx_u64 x = ld64(p) ^ ld64(m);
        if (x) return (cx_u32)(p - start) + ((cx_u32)__builtin_ctzll(x) >> 3);
        p += 8;
        m += 8;
    }
    while (p < end && *p == *m) {
        p++;
        m++;
    }
    return (cx_u32)(p - start);
}

/* --- costs ----------------------------------------------------------------------------------- */

/* 256 log2(x), from a table of the mantissa's log for its top 8 bits: within 1/180 of a bit. */
static cx_u16 FRAC[256];

static inline cx_u32 lg8(cx_u32 x) {
    const cx_u32 hb = high_bit(x);
    const cx_u32 m = hb >= 8 ? x >> (hb - 8) : x << (8 - hb);
    return (hb << 8) + FRAC[m - 256];
}

/* c lg c, in 1/256 bits, for the counts most often met. */
#define CLG_N 4096
static cx_u32 CLG[CLG_N];

/* --- tables ---------------------------------------------------------------------------------- */

static const cx_u32 LL_BASE[LL_MAX + 1] = {0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   10,   11,   12,   13,   14,    15,    16,    18,
                                            20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const cx_u8 LL_EXTRA[LL_MAX + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const cx_u32 ML_BASE[ML_MAX + 1] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  12,  13,  14,  15,  16,  17,   18,   19,   20,
                                            21, 22, 23, 24, 25, 26, 27, 28, 29,  30,  31,  32,  33,  34,  35,   37,   39,   41,
                                            43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const cx_u8 ML_EXTRA[ML_MAX + 1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* The predefined distributions. */
static const cx_i16 LL_NORM[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const cx_i16 ML_NORM[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const cx_i16 OF_NORM[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

/* A literal length's code, below 64, and a match length's less 3, below 128. */
static cx_u8 LL_CODE[64];
static cx_u8 ML_CODE[128];

static inline cx_u32 ll_code(cx_u32 ll) { return ll < 64 ? LL_CODE[ll] : high_bit(ll) + 19; }
static inline cx_u32 ml_code(cx_u32 mb) { return mb < 128 ? ML_CODE[mb] : high_bit(mb) + 36; }

/* --- writing bits ---------------------------------------------------------------------------- */

/* Bits written from the low end up, which a decoder reads back from the end: what's written last
 * is read first. Stores are 8 bytes wide, so the writer stops 8 bytes short of its room, and says
 * so if it would have gone past. */
typedef struct {
    cx_u64 acc;
    cx_u32 n;
    cx_u8* p;
    cx_u8* start;
    cx_u8* end;
    int over;
} bw_t;

static inline void bw_init(bw_t* w, cx_u8* p, cx_usize cap) {
    w->acc = 0;
    w->n = 0;
    w->p = p;
    w->start = p;
    w->end = cap >= 8 ? p + cap - 8 : p;
    w->over = cap < 8;
}

/* Adds the low nb bits of v, which has none above them; at most 64 bits may be held. */
static inline void bw_add(bw_t* w, cx_u64 v, cx_u32 nb) {
    w->acc |= v << w->n;
    w->n += nb;
}

static inline void bw_flush(bw_t* w) {
    const cx_u32 k = w->n >> 3;
    st64(w->p, w->acc);
    w->p += k;
    w->acc = k ? (k == 8 ? 0 : w->acc >> (k << 3)) : w->acc;
    w->n &= 7;
    if (w->p > w->end) {
        w->p = w->end;
        w->over = 1;
    }
}

/* Ends the stream with its mark: gives its size, or 0 if it didn't fit. */
static cx_usize bw_close(bw_t* w) {
    bw_add(w, 1, 1);
    bw_flush(w);
    if (w->over) return 0;
    return (cx_usize)(w->p - w->start) + (w->n > 0);
}

/* --- FSE ------------------------------------------------------------------------------------- */

/* An encoding table: st holds the states in the order the decoder's table spreads them, size + u
 * for its state u, grouped by symbol; dnb and dfs take a state to how many bits it gives up and
 * where its next one is, for each symbol, and first is the state a stream starts in for it. */
typedef struct {
    cx_u16 st[1 << LL_LOG];
    cx_u32 dnb[64];
    cx_i32 dfs[64];
    cx_u16 first[64];
} ct_t;

/* Builds the encoding table for norm[0..max] at accuracy `log`, as the decoder spreads it. */
static void build_ct(ct_t* ct, const cx_i16* norm, cx_u32 max, cx_u32 log) {
    const cx_u32 size = 1u << log, mask = size - 1;
    cx_u8 sym[1 << LL_LOG];
    cx_u32 cumul[65], at[64];
    cx_u32 high = size - 1;
    cumul[0] = 0;
    for (cx_u32 s = 0; s <= max; s++) {
        cumul[s + 1] = cumul[s] + (norm[s] == -1 ? 1 : (cx_u32)norm[s]);
        if (norm[s] == -1) sym[high--] = (cx_u8)s;
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
    for (cx_u32 s = 0; s <= max; s++) at[s] = cumul[s];
    for (cx_u32 u = 0; u < size; u++) ct->st[at[sym[u]]++] = (cx_u16)(size + u);
    for (cx_u32 s = 0; s <= max; s++) {
        const int c = norm[s];
        if (c == 0) {
            ct->dnb[s] = 0;
            ct->dfs[s] = 0;
            ct->first[s] = (cx_u16)size;
        } else if (c == -1 || c == 1) {
            ct->dnb[s] = (log << 16) - size;
            ct->dfs[s] = (cx_i32)cumul[s] - 1;
            ct->first[s] = ct->st[cumul[s]];
        } else {
            const cx_u32 mb = log - high_bit((cx_u32)c - 1);
            ct->dnb[s] = (mb << 16) - ((cx_u32)c << mb);
            ct->dfs[s] = (cx_i32)cumul[s] - c;
            ct->first[s] = ct->st[cumul[s]];
        }
    }
}

/* The table for one symbol repeated: no bits at all. */
static void build_rle(ct_t* ct, cx_u32 s) {
    ct->st[0] = 1;
    ct->dnb[s] = 0;
    ct->dfs[s] = -1;
    ct->first[s] = 1;
}

/* Moves the state on to one that gives symbol s, writing the bits that lead back. */
static inline void ct_put(bw_t* w, const ct_t* ct, cx_u32* x, cx_u32 s) {
    const cx_u32 nb = (*x + ct->dnb[s]) >> 16;
    bw_add(w, *x & ((1u << nb) - 1), nb);
    *x = ct->st[(cx_i32)(*x >> nb) + ct->dfs[s]];
}

/* Probabilities for count[0..max] over `total`, summing to 2^log, each symbol that's there at least
 * 1: each rounded, then nudged a slot at a time where it costs least, by count / slots. Gives what
 * the counts cost with them. */
static cx_u64 normalize(const cx_u32* count, cx_u32 max, cx_u32 total, cx_u32 log, cx_i16* norm) {
    const cx_u32 size = 1u << log;
    /* The symbols there are, in order, so the passes below go over only them, and ties go to the
     * first as before. A count is at most 2^17 and size 2^9, so the rounding fits in 32 bits. */
    cx_u32 sym[64], cnt[64], nv[64];
    cx_u32 k = 0;
    cx_i32 sum = 0;
    for (cx_u32 s = 0; s <= max; s++) {
        norm[s] = 0;
        if (count[s] == 0) continue;
        cx_u32 n = (count[s] * size + total / 2) / total;
        if (n < 1) n = 1;
        sym[k] = s;
        cnt[k] = count[s];
        nv[k++] = n;
        sum += (cx_i32)n;
    }
    while (sum > (cx_i32)size) {
        /* Take a slot from the symbol it costs least, count / (n - 1/2), starting from 1/0. There's
         * always one with more than 1, as there are no more symbols than slots. */
        cx_u32 best = 0;
        cx_u64 bc = 1, bn = 0;
        for (cx_u32 j = 0; j < k; j++) {
            const cx_u64 c = cnt[j], n = 2 * (cx_u64)nv[j] - 1;
            const int take = nv[j] > 1 && c * bn < bc * n;
            best = take ? j : best;
            bc = take ? c : bc;
            bn = take ? n : bn;
        }
        nv[best]--;
        sum--;
    }
    while (sum < (cx_i32)size) {
        /* Give one to the symbol it helps most, count / (n + 1/2). */
        cx_u32 best = 0;
        cx_u64 bc = 0, bn = 1;
        for (cx_u32 j = 0; j < k; j++) {
            const cx_u64 c = cnt[j], n = 2 * (cx_u64)nv[j] + 1;
            const int give = c * bn > bc * n;
            best = give ? j : best;
            bc = give ? c : bc;
            bn = give ? n : bn;
        }
        nv[best]++;
        sum++;
    }
    /* And what the counts cost with them, in 1/256 bits. */
    cx_u64 cost = 0;
    for (cx_u32 j = 0; j < k; j++) {
        norm[sym[j]] = (cx_i16)nv[j];
        cost += (cx_u64)cnt[j] * ((log << 8) - lg8(nv[j]));
    }
    return cost;
}

/* Writes norm[0..max] as the decoder reads a table's description: gives the bytes. */
static cx_usize write_counts(const cx_i16* norm, cx_u32 max, cx_u32 log, cx_u8* out) {
    cx_u64 acc = log - 5;
    cx_u32 n = 4;
    cx_u8* p = out;
#define PUT(v, nb)                \
    do {                          \
        acc |= (cx_u64)(v) << n;  \
        n += (nb);                \
        while (n >= 8) {          \
            *p++ = (cx_u8)acc;    \
            acc >>= 8;            \
            n -= 8;               \
        }                         \
    } while (0)
    int remaining = (1 << log) + 1, threshold = 1 << log;
    cx_u32 nbits = log + 1, s = 0;
    int zero = 0;
    while (remaining > 1 && s <= max) {
        if (zero) {
            const cx_u32 from = s;
            while (norm[s] == 0) s++;
            cx_u32 z = s - from;
            while (z >= 3) {
                PUT(3, 2);
                z -= 3;
            }
            PUT(z, 2);
        }
        const int c = norm[s++];
        const int v = c + 1;
        const int most = 2 * threshold - 1 - remaining;
        if (v < most) {
            PUT(v, nbits - 1);
        } else if (v < threshold) {
            PUT(v, nbits);
        } else {
            PUT(v + most, nbits);
        }
        remaining -= c < 0 ? -c : c;
        zero = c == 0;
        while (remaining < threshold) {
            nbits--;
            threshold >>= 1;
        }
    }
    if (n) *p++ = (cx_u8)acc;
#undef PUT
    return (cx_usize)(p - out);
}

/* --- Huffman --------------------------------------------------------------------------------- */

/* Code lengths for count[0..max], two symbols or more, none longer than HUF_LOG: a Huffman code,
 * by Moffat and Katajainen's way of working it out in place from the counts in order, and then, if
 * it's too deep, the deepest leaves lifted and others pushed down until it fits. Gives the longest. */
static cx_u32 huf_lengths(const cx_u32* count, cx_u32 max, cx_u8* len) {
    cx_u32 sym[256];
    cx_i64 a[256];
    int n = 0;
    for (cx_u32 s = 0; s <= max; s++) {
        len[s] = 0;
        if (count[s]) sym[n++] = s;
    }
    /* In order of count, rarest first, and by symbol where counts are the same: a stable counting
     * sort, by the whole count when they're all below 512, as a small block's are, and otherwise
     * two passes of nine bits each, as a block's counts are below 2^18. */
    {
        cx_u32 most = 0;
        for (int i = 0; i < n; i++) most = count[sym[i]] > most ? count[sym[i]] : most;
        cx_u32 tmp[256];
        const int passes = most < 512 ? 1 : 2;
        const cx_u32 buckets = passes == 1 ? most + 1 : 512;
        for (int pass = 0; pass < passes; pass++) {
            const int sh = 9 * pass;
            cx_u32 at[513];
            memset(at, 0, (buckets + 1) * sizeof(cx_u32));
            cx_u32* const from = pass ? tmp : sym;
            cx_u32* const to = pass ? sym : tmp;
            for (int i = 0; i < n; i++) at[((count[from[i]] >> sh) & 511) + 1]++;
            for (cx_u32 k = 0; k < buckets; k++) at[k + 1] += at[k];
            for (int i = 0; i < n; i++) to[at[(count[from[i]] >> sh) & 511]++] = from[i];
        }
        if (passes == 1) memcpy(sym, tmp, (cx_usize)n * sizeof(cx_u32));
    }
    for (int i = 0; i < n; i++) a[i] = count[sym[i]];
    /* The tree: each internal node's weight, then its parent. */
    int s = 0, r = 0;
    for (int t = 0; t < n - 1; t++) {
        if (s >= n || (r < t && a[r] < a[s])) {
            a[t] = a[r];
            a[r++] = t;
        } else {
            a[t] = a[s++];
        }
        if (s >= n || (r < t && a[r] < a[s])) {
            a[t] += a[r];
            a[r++] = t;
        } else {
            a[t] += a[s++];
        }
    }
    /* Each internal node's depth, then each leaf's. */
    a[n - 2] = 0;
    for (int t = n - 3; t >= 0; t--) a[t] = a[a[t]] + 1;
    {
        int avail = 1, used = 0, d = 0, t = n - 2, x = n - 1;
        while (avail > 0) {
            while (t >= 0 && a[t] == d) {
                used++;
                t--;
            }
            while (avail > used) {
                a[x--] = d;
                avail--;
            }
            avail = 2 * used;
            d++;
            used = 0;
        }
    }
    /* a[0] is the rarest and so the deepest. */
    if (a[0] <= HUF_LOG) {
        for (int i = 0; i < n; i++) len[sym[i]] = (cx_u8)a[i];
        return (cx_u32)a[0];
    }
    cx_u32 bl[HUF_LOG + 1] = {0};
    for (int i = 0; i < n; i++) bl[a[i] > HUF_LOG ? HUF_LOG : a[i]]++;
    const cx_i64 target = (cx_i64)1 << HUF_LOG;
    cx_i64 k = 0;
    for (cx_u32 d = 1; d <= HUF_LOG; d++) k += (cx_i64)bl[d] << (HUF_LOG - d);
    while (k > target) {
        cx_u32 d = HUF_LOG - 1;
        while (bl[d] == 0) d--;
        bl[d]--;
        bl[d + 1]++;
        k -= (cx_i64)1 << (HUF_LOG - d - 1);
    }
    while (k < target) {
        cx_u32 d = HUF_LOG;
        while (bl[d] == 0) d--;
        bl[d]--;
        bl[d - 1]++;
        k += (cx_i64)1 << (HUF_LOG - d);
    }
    int i = 0;
    cx_u32 longest = 0;
    for (cx_u32 d = HUF_LOG; d >= 1; d--) {
        for (cx_u32 c = 0; c < bl[d]; c++) len[sym[i++]] = (cx_u8)d;
        if (bl[d] && !longest) longest = d;
    }
    return longest;
}

/* The codes for lengths len[0..max] with the longest `log`, laid out as the decoder lays its table
 * out: each weight's symbols in order, the longest codes first. Each is code | bits << 16. */
static void huf_codes(const cx_u8* len, cx_u32 max, cx_u32 log, cx_u32* ct) {
    /* Where each length's codes start: the longest first, each length's symbols in order. */
    cx_u32 n[HUF_LOG + 2] = {0}, pos[HUF_LOG + 2];
    for (cx_u32 s = 0; s <= max; s++) n[len[s]]++;
    pos[log] = 0;
    for (cx_u32 l = log; l > 1; l--) pos[l - 1] = pos[l] + (n[l] << (log - l));
    memset(ct, 0, 256 * sizeof(cx_u32));
    for (cx_u32 s = 0; s <= max; s++) {
        const cx_u32 l = len[s];
        if (!l) continue;
        ct[s] = (pos[l] >> (log - l)) | (l << 16);
        pos[l] += 1u << (log - l);
    }
}

/* Writes the weights w[0..n), n of 2 or more with two different weights at least, FSE-coded by two
 * states taking turns, as the decoder reads them: gives the bytes, or 0 if it's over 127. */
static cx_usize fse_weights(const cx_u8* w, cx_u32 n, cx_u8* out) {
    cx_u32 count[16] = {0}, max = 0;
    for (cx_u32 i = 0; i < n; i++) {
        count[w[i]]++;
        if (w[i] > max) max = w[i];
    }
    cx_u8 best[160];
    cx_usize best_n = 0;
    for (cx_u32 log = 5; log <= 6; log++) {
        cx_i16 norm[16];
        normalize(count, max, n, log, norm);
        cx_u8 tmp[160];
        cx_usize at = write_counts(norm, max, log, tmp);
        ct_t ct;
        build_ct(&ct, norm, max, log);
        bw_t b;
        bw_init(&b, tmp + at, sizeof tmp - at);
        cx_u32 x[2];
        x[(n - 1) & 1] = ct.first[w[n - 1]];
        x[(n - 2) & 1] = ct.first[w[n - 2]];
        for (int k = (int)n - 3; k >= 0; k--) {
            ct_put(&b, &ct, &x[k & 1], w[k]);
            bw_flush(&b);
        }
        bw_add(&b, x[1] - (1u << log), log);
        bw_add(&b, x[0] - (1u << log), log);
        const cx_usize bits = bw_close(&b);
        if (!bits) continue;
        const cx_usize total = at + bits;
        if (total < 128 && (!best_n || total < best_n)) {
            memcpy(best, tmp, total);
            best_n = total;
        }
    }
    if (best_n) memcpy(out, best, best_n);
    return best_n;
}

/* Writes the tree for lengths len[0..max], max being the last symbol with a code, longest `log`:
 * its weights but the last's, FSE-coded or four bits each, whichever is smaller. 0 if neither will
 * do, which needs a byte for every symbol up to 255 all alike. */
static cx_usize huf_tree(const cx_u8* len, cx_u32 max, cx_u32 log, cx_u8* out) {
    cx_u8 w[256];
    const cx_u32 n = max;
    int alike = 1;
    for (cx_u32 s = 0; s < n; s++) {
        w[s] = len[s] ? (cx_u8)(log + 1 - len[s]) : 0;
        if (w[s] != w[0]) alike = 0;
    }
    cx_u8 tmp[160];
    const cx_usize fse = n >= 2 && !alike ? fse_weights(w, n, tmp) : 0;
    const cx_usize raw = n <= 128 ? 1 + (n + 1) / 2 : 0;
    if (fse && (!raw || fse + 1 < raw)) {
        out[0] = (cx_u8)fse;
        memcpy(out + 1, tmp, fse);
        return 1 + fse;
    }
    if (!raw) return 0;
    out[0] = (cx_u8)(127 + n);
    for (cx_u32 i = 0; i < n; i += 2) out[1 + i / 2] = (cx_u8)((w[i] << 4) | (i + 1 < n ? w[i + 1] : 0));
    return raw;
}

/* One stream of literals src[0..n), last first so the decoder reads them in order: gives its size,
 * or 0 if it doesn't fit in cap. */
static cx_usize huf_stream(const cx_u32* ct, const cx_u8* src, cx_usize n, cx_u8* out, cx_usize cap) {
    bw_t b;
    bw_init(&b, out, cap);
    cx_usize i = n;
    while (i >= 4) {
        const cx_u32 e0 = ct[src[i - 1]], e1 = ct[src[i - 2]], e2 = ct[src[i - 3]], e3 = ct[src[i - 4]];
        bw_add(&b, e0 & 0xFFFF, e0 >> 16);
        bw_add(&b, e1 & 0xFFFF, e1 >> 16);
        bw_add(&b, e2 & 0xFFFF, e2 >> 16);
        bw_add(&b, e3 & 0xFFFF, e3 >> 16);
        bw_flush(&b);
        i -= 4;
    }
    while (i > 0) {
        const cx_u32 e = ct[src[--i]];
        bw_add(&b, e & 0xFFFF, e >> 16);
    }
    bw_flush(&b);
    return bw_close(&b);
}

/* Room for four streams of a block's literals, each its most: 11 bits a symbol, and a word over. */
#define HUF_ROOM ((BLOCK_MAX / 4 + 1) * 11 / 8 + 16)

/* The literals in one stream, or four with their sizes first: gives the bytes, or 0. The four are
 * written at once, each in its own room in tmp (4 * HUF_ROOM bytes), so their bits go in side by
 * side rather than one stream after another; then they're put together in out. */
static cx_usize huf_streams(const cx_u32* ct, const cx_u8* src, cx_usize n, int four, cx_u8* out, cx_usize cap, cx_u8* tmp) {
    if (!four) return huf_stream(ct, src, n, out, cap);
    if (cap < 6) return 0;
    const cx_usize seg = (n + 3) / 4, last = n - 3 * seg;
    const cx_u8* const s0 = src;
    const cx_u8* const s1 = src + seg;
    const cx_u8* const s2 = src + 2 * seg;
    const cx_u8* const s3 = src + 3 * seg;
    cx_u8* const t[4] = {tmp, tmp + HUF_ROOM, tmp + 2 * HUF_ROOM, tmp + 3 * HUF_ROOM};
    cx_u8 *p0 = t[0], *p1 = t[1], *p2 = t[2], *p3 = t[3];
    cx_u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    cx_u32 n0 = 0, n1 = 0, n2 = 0, n3 = 0;
#define HPUT(a, nb, s, i)                     \
    do {                                      \
        const cx_u32 e_ = ct[(s)[i]];         \
        (a) |= (cx_u64)(e_ & 0xFFFF) << (nb); \
        (nb) += e_ >> 16;                     \
    } while (0)
#define HFLUSH(a, nb, p)        \
    do {                        \
        st64((p), (a));         \
        (p) += (nb) >> 3;       \
        (a) >>= (nb) & ~7u;     \
        (nb) &= 7;              \
    } while (0)
    /* The first three streams' symbols past the fourth's length, then all four together. */
    cx_usize i = seg;
    while (i > last) {
        i--;
        HPUT(a0, n0, s0, i);
        HPUT(a1, n1, s1, i);
        HPUT(a2, n2, s2, i);
    }
    HFLUSH(a0, n0, p0);
    HFLUSH(a1, n1, p1);
    HFLUSH(a2, n2, p2);
    while (i >= 4) {
        i -= 4;
        HPUT(a0, n0, s0, i + 3);
        HPUT(a1, n1, s1, i + 3);
        HPUT(a2, n2, s2, i + 3);
        HPUT(a3, n3, s3, i + 3);
        HPUT(a0, n0, s0, i + 2);
        HPUT(a1, n1, s1, i + 2);
        HPUT(a2, n2, s2, i + 2);
        HPUT(a3, n3, s3, i + 2);
        HPUT(a0, n0, s0, i + 1);
        HPUT(a1, n1, s1, i + 1);
        HPUT(a2, n2, s2, i + 1);
        HPUT(a3, n3, s3, i + 1);
        HPUT(a0, n0, s0, i);
        HPUT(a1, n1, s1, i);
        HPUT(a2, n2, s2, i);
        HPUT(a3, n3, s3, i);
        HFLUSH(a0, n0, p0);
        HFLUSH(a1, n1, p1);
        HFLUSH(a2, n2, p2);
        HFLUSH(a3, n3, p3);
    }
    while (i > 0) {
        i--;
        HPUT(a0, n0, s0, i);
        HPUT(a1, n1, s1, i);
        HPUT(a2, n2, s2, i);
        HPUT(a3, n3, s3, i);
    }
    /* Each ends with its mark. */
    cx_usize got[4];
    cx_u8* const p[4] = {p0, p1, p2, p3};
    const cx_u64 a[4] = {a0, a1, a2, a3};
    const cx_u32 nb[4] = {n0, n1, n2, n3};
    for (int k = 0; k < 4; k++) {
        cx_u64 acc = a[k] | (cx_u64)1 << nb[k];
        cx_u32 m = nb[k] + 1;
        cx_u8* q = p[k];
        HFLUSH(acc, m, q);
        got[k] = (cx_usize)(q - t[k]) + (m > 0);
    }
#undef HPUT
#undef HFLUSH
    if (got[0] > 0xFFFF || got[1] > 0xFFFF || got[2] > 0xFFFF) return 0;
    if (cap < 6 + got[0] + got[1] + got[2] + got[3]) return 0;
    cx_usize at = 6;
    for (int k = 0; k < 4; k++) {
        if (k < 3) st16(out + 2 * k, (cx_u32)got[k]);
        memcpy(out + at, t[k], got[k]);
        at += got[k];
    }
    return at;
}

/* --- the encoder's state --------------------------------------------------------------------- */

enum { T_NONE, T_PRE, T_RLE, T_FSE };

/* A table the sequences were coded with, which the next block may repeat. cost[s] is what a code
 * s costs with it in 1/256 bits, or COST_BAD for one it can't code. */
typedef struct {
    cx_u32 kind, log, max, rle;
    cx_i16 norm[64];
    cx_u32 cost[64];
    ct_t ct;
} tab_t;

/* Above what any block's codes can cost: 2^17 of them at 9 bits each is under 2^29. */
#define COST_BAD (1u << 31)

static void tab_costs(tab_t* t) {
    for (cx_u32 s = 0; s < 64; s++) {
        const cx_i32 c = s <= t->max ? t->norm[s] : 0;
        t->cost[s] = c == 0 ? COST_BAD : (t->log << 8) - lg8(c < 0 ? 1 : (cx_u32)c);
    }
}

/* What count[0..max] costs with t, in 1/256 bits, or ~0 if t lacks a symbol there is: without a
 * branch for each symbol, as most counts are 0. */
static cx_u64 tab_cost(const tab_t* t, const cx_u32* count, cx_u32 max) {
    if (max > t->max) return ~(cx_u64)0;
    cx_u64 c = 0;
    for (cx_u32 s = 0; s <= max; s++) c += (cx_u64)count[s] * t->cost[s];
    return c >= COST_BAD ? ~(cx_u64)0 : c;
}

/* What a block leaves for the next: its Huffman codes, its three tables, its repeated offsets. */
typedef struct {
    cx_u32 huf_ok, huf_log;
    cx_u32 huf[256]; /* code | bits << 16, or 0 for a symbol without one */
    tab_t ll, of, ml;
    cx_u32 rep[3];
} ent_t;

/* A sequence: its literals' length, its match's length, and its offset as the match finder coded
 * it, 1 to 3 for its repeated offsets and the distance plus 3 for a new one. If its block's
 * repeated offsets aren't the ones the finder had, as after a block that was stored, the offsets
 * are coded again when the block is. */
typedef struct {
    cx_u32 ll, ml, ofb;
} seq_t;

#define SEQ_MAX (BLOCK_MAX / 3 + 1)

typedef struct {
    seq_t* seq;
    cx_u32 n;
    cx_u8* lit;
    cx_u8* lp;
    const cx_u8* src_end; /* where the text ends: literals are copied 16 at a time before it */
} ss_t;

/* The offset `off` after `ll` literals as coded, 1 to 3 for a repeated one and off + 3 for a new
 * one, with the repeated offsets r moved on as the decoder moves them. */
static inline cx_u32 rep_code(cx_u32* r, cx_u32 ll, cx_u32 off) {
    cx_u32 ofb;
    if (ll) {
        if (off == r[0]) {
            ofb = 1;
        } else if (off == r[1]) {
            r[1] = r[0];
            r[0] = off;
            ofb = 2;
        } else if (off == r[2]) {
            r[2] = r[1];
            r[1] = r[0];
            r[0] = off;
            ofb = 3;
        } else {
            goto fresh;
        }
    } else {
        if (off == r[1]) {
            r[1] = r[0];
            r[0] = off;
            ofb = 1;
        } else if (off == r[2]) {
            r[2] = r[1];
            r[1] = r[0];
            r[0] = off;
            ofb = 2;
        } else if (off == r[0] - 1) {
            r[2] = r[1];
            r[1] = r[0];
            r[0] = off;
            ofb = 3;
        } else {
            goto fresh;
        }
    }
    goto done;
fresh:
    r[2] = r[1];
    r[1] = r[0];
    r[0] = off;
    ofb = off + 3;
done:
    return ofb;
}

/* The literals within 16 bytes of the text's end, which the copy in ss_put would read past. */
static __attribute__((noinline, cold)) void lit_copy_end(cx_u8* d, const cx_u8* lit, cx_u32 ll) {
    memcpy(d, lit, ll);
}

/* Takes a sequence: its literals copied out, and the match finder's repeated offsets r moved on.
 * They're copied 16 bytes at a time, which can write 15 past them, as the literal buffer has the
 * room for: a run longer than 16 takes a few more copies rather than a call. */
static inline void ss_put(ss_t* s, cx_u32* r, const cx_u8* lit, cx_u32 ll, cx_u32 off, cx_u32 ml) {
    cx_u8* d = s->lp;
    if (__builtin_expect(lit + ll + 16 <= s->src_end, 1)) {
        memcpy(d, lit, 16);
        for (cx_u32 i = 16; i < ll; i += 16) memcpy(d + i, lit + i, 16);
    } else {
        lit_copy_end(d, lit, ll);
    }
    s->lp = d + ll;
#ifdef ZSE_CHECK
    {
        const cx_u8* m = lit + ll;
        for (cx_u32 i = 0; i < ml; i++) {
            if (m[i] != m[(ptrdiff_t)i - (ptrdiff_t)off]) {
                fprintf(stderr, "bad match: %u long, %u back, wrong at %u, %u literals before\n", ml, off, i, ll);
                abort();
            }
        }
        if (m + ml > s->src_end || ml < 3 || off == 0) {
            fprintf(stderr, "bad match: %u long, %u back\n", ml, off);
            abort();
        }
    }
#endif
    seq_t* q = &s->seq[s->n++];
    q->ll = ll;
    q->ml = ml;
    q->ofb = rep_code(r, ll, off);
}

/* The distance an offset coded as ofb after ll literals is, with the repeated offsets r moved on:
 * as the decoder works it out. */
static inline cx_u32 rep_decode(cx_u32* r, cx_u32 ll, cx_u32 ofb) {
    if (ofb > 3) {
        r[2] = r[1];
        r[1] = r[0];
        r[0] = ofb - 3;
        return r[0];
    }
    const cx_u32 k = ofb - 1 + (ll == 0);
    if (k == 0) return r[0];
    const cx_u32 off = k == 3 ? r[0] - 1 : k == 1 ? r[1] : r[2];
    if (k != 1) r[2] = r[1];
    r[1] = r[0];
    r[0] = off;
    return off;
}

/* The literals after the last match. */
static inline void ss_tail(ss_t* s, const cx_u8* lit, cx_usize n) {
    memcpy(s->lp, lit, n);
    s->lp += n;
}

enum { S_FAST = 1, S_DFAST, S_GREEDY, S_LAZY, S_LAZY2, S_BTLAZY2, S_BTOPT, S_BTULTRA, S_BTULTRA2 };

/* A level's settings: the window, the chain (or second hash, or tree), the hash, how deep to
 * search, the shortest match, the length that's enough (or, for the negative levels, how fast to
 * skip), and the match finder. */
typedef struct {
    cx_u32 wlog, clog, hlog, slog, mml, tlen, strat;
} cp_t;

/* By the input's size, with a dictionary's content counted in, as libzstd picks: over 256 KiB, up
 * to 256 KiB, up to 128 KiB and up to 16 KiB. */
static const cp_t LEVELS[4][23] = {{
    {19, 12, 13, 1, 6, 1, S_FAST},       /* the negative levels, with tlen their speed */
    {19, 13, 14, 1, 7, 0, S_FAST},       /* 1 */
    {20, 15, 16, 1, 6, 0, S_FAST},       /* 2 */
    {21, 16, 17, 1, 5, 0, S_DFAST},      /* 3 */
    {21, 18, 18, 1, 5, 0, S_DFAST},      /* 4 */
    {21, 18, 19, 3, 5, 2, S_GREEDY},     /* 5 */
    {21, 18, 19, 3, 5, 4, S_LAZY},       /* 6 */
    {21, 19, 20, 4, 5, 8, S_LAZY},       /* 7 */
    {21, 19, 20, 4, 5, 16, S_LAZY2},     /* 8 */
    {22, 20, 21, 4, 5, 16, S_LAZY2},     /* 9 */
    {22, 21, 22, 5, 5, 16, S_LAZY2},     /* 10 */
    {22, 21, 22, 6, 5, 16, S_LAZY2},     /* 11 */
    {22, 22, 23, 6, 5, 32, S_LAZY2},     /* 12 */
    {22, 22, 22, 4, 4, 32, S_BTLAZY2},   /* 13 */
    {22, 22, 23, 5, 4, 32, S_BTLAZY2},   /* 14 */
    {22, 23, 23, 6, 5, 32, S_BTLAZY2},   /* 15 */
    {22, 22, 22, 5, 5, 48, S_BTOPT},     /* 16 */
    {23, 23, 22, 5, 4, 64, S_BTOPT},     /* 17 */
    {23, 23, 22, 6, 3, 64, S_BTULTRA},   /* 18 */
    {23, 24, 22, 7, 3, 256, S_BTULTRA2}, /* 19 */
    {25, 25, 23, 7, 3, 256, S_BTULTRA2}, /* 20 */
    {26, 26, 24, 7, 3, 512, S_BTULTRA2}, /* 21 */
    {27, 27, 25, 9, 3, 999, S_BTULTRA2}, /* 22 */
}, {
    {18, 12, 13, 1, 5, 1, S_FAST},
    {18, 13, 14, 1, 6, 0, S_FAST},
    {18, 14, 14, 1, 5, 0, S_DFAST},
    {18, 16, 16, 1, 4, 0, S_DFAST},
    {18, 16, 17, 3, 5, 2, S_GREEDY},
    {18, 17, 18, 5, 5, 2, S_GREEDY},
    {18, 18, 19, 3, 5, 4, S_LAZY},
    {18, 18, 19, 4, 4, 4, S_LAZY},
    {18, 18, 19, 4, 4, 8, S_LAZY2},
    {18, 18, 19, 5, 4, 8, S_LAZY2},
    {18, 18, 19, 6, 4, 8, S_LAZY2},
    {18, 18, 19, 5, 4, 12, S_BTLAZY2},
    {18, 19, 19, 7, 4, 12, S_BTLAZY2},
    {18, 18, 19, 4, 4, 16, S_BTOPT},
    {18, 18, 19, 4, 3, 32, S_BTOPT},
    {18, 18, 19, 6, 3, 128, S_BTOPT},
    {18, 19, 19, 6, 3, 128, S_BTULTRA},
    {18, 19, 19, 8, 3, 256, S_BTULTRA},
    {18, 19, 19, 6, 3, 128, S_BTULTRA2},
    {18, 19, 19, 8, 3, 256, S_BTULTRA2},
    {18, 19, 19, 10, 3, 512, S_BTULTRA2},
    {18, 19, 19, 12, 3, 512, S_BTULTRA2},
    {18, 19, 19, 13, 3, 999, S_BTULTRA2},
}, {
    {17, 12, 12, 1, 5, 1, S_FAST},
    {17, 12, 13, 1, 6, 0, S_FAST},
    {17, 13, 15, 1, 5, 0, S_FAST},
    {17, 15, 16, 2, 5, 0, S_DFAST},
    {17, 17, 17, 2, 4, 0, S_DFAST},
    {17, 16, 17, 3, 4, 2, S_GREEDY},
    {17, 16, 17, 3, 4, 4, S_LAZY},
    {17, 16, 17, 3, 4, 8, S_LAZY2},
    {17, 16, 17, 4, 4, 8, S_LAZY2},
    {17, 16, 17, 5, 4, 8, S_LAZY2},
    {17, 16, 17, 6, 4, 8, S_LAZY2},
    {17, 17, 17, 5, 4, 8, S_BTLAZY2},
    {17, 18, 17, 7, 4, 12, S_BTLAZY2},
    {17, 18, 17, 3, 4, 12, S_BTOPT},
    {17, 18, 17, 4, 3, 32, S_BTOPT},
    {17, 18, 17, 6, 3, 256, S_BTOPT},
    {17, 18, 17, 6, 3, 128, S_BTULTRA},
    {17, 18, 17, 8, 3, 256, S_BTULTRA},
    {17, 18, 17, 10, 3, 512, S_BTULTRA},
    {17, 18, 17, 5, 3, 256, S_BTULTRA2},
    {17, 18, 17, 7, 3, 512, S_BTULTRA2},
    {17, 18, 17, 9, 3, 512, S_BTULTRA2},
    {17, 18, 17, 11, 3, 999, S_BTULTRA2},
}, {
    {14, 12, 13, 1, 5, 1, S_FAST},
    {14, 14, 15, 1, 5, 0, S_FAST},
    {14, 14, 15, 1, 4, 0, S_FAST},
    {14, 14, 15, 2, 4, 0, S_DFAST},
    {14, 14, 14, 4, 4, 2, S_GREEDY},
    {14, 14, 14, 3, 4, 4, S_LAZY},
    {14, 14, 14, 4, 4, 8, S_LAZY2},
    {14, 14, 14, 6, 4, 8, S_LAZY2},
    {14, 14, 14, 8, 4, 8, S_LAZY2},
    {14, 15, 14, 5, 4, 8, S_BTLAZY2},
    {14, 15, 14, 9, 4, 8, S_BTLAZY2},
    {14, 15, 14, 3, 4, 12, S_BTOPT},
    {14, 15, 14, 4, 3, 24, S_BTOPT},
    {14, 15, 14, 5, 3, 32, S_BTULTRA},
    {14, 15, 15, 6, 3, 64, S_BTULTRA},
    {14, 15, 15, 7, 3, 256, S_BTULTRA},
    {14, 15, 15, 5, 3, 48, S_BTULTRA2},
    {14, 15, 15, 6, 3, 128, S_BTULTRA2},
    {14, 15, 15, 7, 3, 256, S_BTULTRA2},
    {14, 15, 15, 8, 3, 256, S_BTULTRA2},
    {14, 15, 15, 8, 3, 512, S_BTULTRA2},
    {14, 15, 15, 9, 3, 512, S_BTULTRA2},
    {14, 15, 15, 10, 3, 999, S_BTULTRA2},
}};

/* A match: its length and how far back. */
typedef struct {
    cx_u32 len, off;
} match_t;

/* How often each literal and each code has come up, for the optimal parser's prices: they carry
 * over from block to block, halved at each, and move on with each sequence taken. */
typedef struct {
    cx_u32 lit[256], ll[LL_MAX + 1], ml[ML_MAX + 1], of[OF_MAX + 1];
    cx_u32 lit_sum, ll_sum, ml_sum, of_sum;
    /* the prices they make, in 1/256 bits */
    cx_u32 lit_p[256], ll_p[LL_MAX + 1], ml_p[ML_MAX + 1], of_p[OF_MAX + 1];
    int ready;
} stats_t;

/* A place the optimal parse can reach: the cheapest way there, by a match of mlen from mlen back
 * at distance off, or by a literal if mlen is 0, with the literals since the last match and the
 * repeated offsets it leaves. */
typedef struct {
    cx_u32 price, off, mlen, litlen;
    cx_u32 rep[3];
} node_t;

#define OPT_NUM 4096
/* The matches noted at a place: the repeated offsets', a three-byte one, and the tree's, which
 * each search notes one of at most. */
#define MATCHES_MAX (8 + 1024)

/* A job: a stretch of the text compressed into blocks of its own, with the text before it to match
 * into. Positions count from `base`, the first byte at START, which is past what a window reaches,
 * so a table's empty 0 is always too far back. */
typedef struct {
    cp_t p;
    const cx_u8* base;
    cx_u32 from, to; /* the job's own text, base[from..to); before it, from START, is what it matches into */
    cx_u32 start;    /* START */
    cx_u32 wsize;
    int last;  /* whether its last block ends the frame */
    int first; /* whether its first block starts it */
    int dict;  /* whether its text before it is a dictionary's content */
    const cx_u8* dparams; /* for the first job, a trained dictionary's tables, or 0 */
    cx_u64 dkey;          /* for the first job, its dictionary's key, or 0 */
    struct digest_s* dg;  /* for the first job, its dictionary's kept tables, held, or 0 */
    struct work_s* dw;    /* and a working copy of them it uses where it is, or 0 */
    /* the match finder's tables */
    cx_u32* hash;
    cx_u32* chain;
    cx_usize hash_n, chain_n;
    cx_u32 next_up; /* the hash rows or trees have every position before this */
    cx_u32 bt_mls;  /* the bytes a tree's hash takes */
    cx_u32 bt_up;   /* how deep a position no match is looked for at goes into its tree */
    cx_u32 lim;     /* where the trees' comparisons stop: `to`, or a dictionary's content's end */
    int cut;        /* whether text that agrees to lim goes in with nothing below it */
    cx_u32 rlog;    /* a hash row's 2^rlog positions */
    cx_u8* tags;    /* each row position's byte more of hash, and each row's head */
    cx_u32* hash3;
    /* With a working copy of a dictionary's trees: each write into a node of a position before ulo,
     * the ones it came with, as its slot and what it held, up to ulog_cap of them; ulog_n is
     * ulog_cap + 1 once there were more. ulo is 0 otherwise, and nothing is noted. */
    cx_u32* ulog;
    cx_u32 ulog_n, ulog_cap, ulo;
    /* the optimal parser's */
    stats_t* st;
    node_t* opt;
    match_t* mt;
    match_t* path;
    cx_u32* path_at;
    /* the block being made, and where it's put together */
    ss_t ss;
    cx_u8* blk;
    ent_t ent[2];
    int cur;
    cx_u32* split; /* the block splitter's counts */
    cx_u8* htmp;   /* where the four Huffman streams are written */
    /* what comes out */
    cx_u8* out;
    cx_usize out_n, out_cap;
    /* A block's codes, written before they're read: last, so a new job clears only what's before
     * them, 13 KiB rather than 310. */
    cx_u8 llc[SEQ_MAX], mlc[SEQ_MAX], ofc[SEQ_MAX];
    cx_u32 ofv[SEQ_MAX];
} job_t;

/* --- the match finders ----------------------------------------------------------------------- */

#define PRIME64 0x9E3779B185EBCA87ull
#define PRIME32 2654435761u

/* The hash of the first `mls` bytes at p, 4 to 8, in hl bits. */
static inline cx_u32 hash_at(const cx_u8* p, cx_u32 mls, cx_u32 hl) {
    if (mls == 4) return (ld32(p) * PRIME32) >> (32 - hl);
    return (cx_u32)(((ld64(p) << (64 - 8 * mls)) * PRIME64) >> (64 - hl));
}

/* Whether a repeated offset r reaches back from position cur into the window: it isn't 0, and
 * there's that much text before. */
#define REP_OK(r, cur, start) ((r) - 1 < (cur) - (start))

/* A table entry m's text, or, if m is out of the window, bytes the text rarely has at `nt`, one of
 * NOT_TEXT's halves: picked without a branch, as whether an entry is in is a coin toss while a
 * table fills, which is all of a small input. The finders compare what it gives with the text
 * first, which seldom agrees and so is a branch that guesses right, and then whether it's
 * base + m, for the text that has NOT_TEXT's bytes; the compiler keeps the two in one branch. An
 * entry out of the window is 0, whose base + m is base, or before the window in the text, where
 * NOT_TEXT isn't, so not_text takes the half that isn't base. On x86-64 the move is written out,
 * as the compiler would otherwise see that an entry out of the window fails the whole test and
 * make the window a branch of its own again. */
static const cx_u8 NOT_TEXT[16] = {0xa5, 0x3c, 0xe7, 0x19, 0x5b, 0xd2, 0x86, 0x4f, 0x3a, 0xc5, 0x7e, 0x91, 0xb5, 0x2d, 0x68, 0xf4};

static inline const cx_u8* not_text(const cx_u8* base) { return base == NOT_TEXT ? NOT_TEXT + 8 : NOT_TEXT; }

static inline const cx_u8* entry_at(const cx_u8* base, cx_u32 m, cx_u32 low, const cx_u8* nt) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    const cx_u8* q = base + m;
    __asm__("cmpl %2, %1\n\tcmovbq %3, %0" : "+r"(q) : "r"(m), "r"(low), "r"(nt) : "cc");
    return q;
#else
    return __builtin_unpredictable(m >= low) ? base + m : nt;
#endif
}

/* One hash table of positions by their first bytes. The search runs a position ahead of itself:
 * the next position's table entry is loaded, and the one after's hash taken, before this one's
 * candidate is compared, so the loads overlap the comparisons. It looks at positions in pairs, a
 * step apart: the step starts at 2, so every position is looked at, and grows by one for each
 * STEP_RUN bytes without a match; the negative levels start it further on. The repeated offset
 * is tried a pair ahead, at the next pair's first position and the one before. A table entry is
 * taken only if it's in the window from all of the block, so one bound serves every position. */
#define STEP_RUN 128

/* Whether table entry m's first four bytes are ip's. With `cmov` the window is tested without a
 * branch, by entry_at; without, by a branch, which guesses right once the table is full and costs
 * the load less waiting. */
#define CAND4(m, ip)                                                                                \
    (cmov ? (ld32(entry_at(base, (m), low, nt)) == ld32(ip) && entry_at(base, (m), low, nt) == base + (m)) \
          : ((m) >= low && ld32(base + (m)) == ld32(ip)))

static inline __attribute__((always_inline)) void mf_fast_with(job_t* J, cx_u32 at, cx_u32 end, cx_u32* rp, const int cmov, const cx_u32 mls) {
    const cx_u8* const base = J->base;
    cx_u32* const ht = J->hash;
    const cx_u32 hl = J->p.hlog, start = J->start;
    const cx_u32 low = end - start > J->wsize ? end - J->wsize : start;
    const cx_u8* const prefix = base + start;
    const cx_u8* const nt = not_text(base);
    /* The repeated offsets and the sequence store are copies of the job's, which nothing the loop
     * stores to the table or the literals can be, so they needn't be read again after each. */
    cx_u32 r[3] = {rp[0], rp[1], rp[2]};
    ss_t sl = J->ss;
    ss_t* const ss = &sl;
    const cx_u8* ip0 = base + at;
    const cx_u8* anchor = ip0;
    const cx_u8* const iend = base + end;
    const cx_u8* const ilimit = iend - READ;
    const cx_usize step0 = J->p.tlen + (J->p.tlen == 0) + 1;
    /* The repeated offset tried: 0, which never matches, while it reaches back out of the text.
     * After any match it's the newest offset, which reaches into the text from there on. */
    cx_u32 rt = REP_OK(r[0], at, start) ? r[0] : 0;
    for (;;) {
        cx_usize step = step0;
        const cx_u8* grow = ip0 + STEP_RUN;
        const cx_u8* ip1 = ip0 + 1;
        const cx_u8* ip2 = ip0 + step;
        const cx_u8* ip3 = ip2 + 1;
        if (ip3 >= ilimit) break;
        cx_u32 h0 = hash_at(ip0, mls, hl), h1 = hash_at(ip1, mls, hl);
        cx_u32 m = ht[h0], c0, ml, off;
        const cx_u8* mp;
        do {
            /* ip0, ip1 = ip0 + 1, ip2 = ip0 + step and ip3 = ip2 + 1; h0 and h1 are ip0's and
             * ip1's hashes, m ip0's entry. */
            const cx_u32 rv = ld32(ip2 - rt);
            c0 = (cx_u32)(ip0 - base);
            ht[h0] = c0;
            if ((ld32(ip2) == rv) & (rt != 0)) {
                ht[h1] = c0 + 1;
                const cx_u32 back = ip2[-1] == (ip2 - rt)[-1];
                ml = count(ip2 + 4, ip2 + 4 - rt, iend) + 4 + back;
                ip0 = ip2 - back;
                off = rt;
                goto found;
            }
            if (CAND4(m, ip0)) {
                ht[h1] = c0 + 1;
                goto fresh;
            }
            m = ht[h1];
            h0 = h1;
            h1 = hash_at(ip2, mls, hl);
            ip0 = ip1;
            ip1 = ip2;
            ip2 = ip3;
            c0 = (cx_u32)(ip0 - base);
            ht[h0] = c0;
            if (CAND4(m, ip0)) {
                /* ip1 is ip0 + step - 1, which the match reaches past only if the step is 4 or
                 * less: the table mustn't hold a position after where the next search starts. */
                if (step <= 4) ht[h1] = (cx_u32)(ip1 - base);
                goto fresh;
            }
            m = ht[h1];
            h0 = h1;
            h1 = hash_at(ip2, mls, hl);
            ip0 = ip1;
            ip1 = ip2;
            ip2 = ip0 + step;
            ip3 = ip1 + step;
            if (ip2 >= grow) {
                step++;
                grow += STEP_RUN;
            }
        } while (ip3 < ilimit);
        break;
    fresh:
        mp = base + m;
        ml = 4;
        while (ip0 > anchor && mp > prefix && ip0[-1] == mp[-1]) {
            ip0--;
            mp--;
            ml++;
        }
        ml += count(ip0 + ml, mp + ml, iend);
        off = (cx_u32)(ip0 - mp);
    found:
        ss_put(ss, r, anchor, (cx_u32)(ip0 - anchor), off, ml);
        ip0 += ml;
        anchor = ip0;
        if (ip0 <= ilimit) {
            ht[hash_at(base + c0 + 2, mls, hl)] = c0 + 2;
            ht[hash_at(ip0 - 2, mls, hl)] = (cx_u32)(ip0 - 2 - base);
            /* The offset before the last, straight after it. */
            for (;;) {
                const cx_u32 c2 = (cx_u32)(ip0 - base);
                if (ip0 > ilimit || !REP_OK(r[1], c2, start) || ld32(ip0) != ld32(ip0 - r[1])) break;
                ml = count(ip0 + 4, ip0 + 4 - r[1], iend) + 4;
                ht[hash_at(ip0, mls, hl)] = c2;
                ss_put(ss, r, anchor, 0, r[1], ml);
                ip0 += ml;
                anchor = ip0;
            }
        }
        rt = r[0];
    }
    ss_tail(ss, anchor, (cx_usize)(iend - anchor));
    J->ss = sl;
    rp[0] = r[0];
    rp[1] = r[1];
    rp[2] = r[2];
}

/* The finders are built once for each length the levels hash, so the hash's shift is a constant
 * and the loop holds no test of the length. */
#define ZSE_BY_MML(f, J, ...)               \
    switch ((J)->p.mml) {                   \
    case 4: f(J, __VA_ARGS__, 4); break;    \
    case 5: f(J, __VA_ARGS__, 5); break;    \
    case 6: f(J, __VA_ARGS__, 6); break;    \
    case 7: f(J, __VA_ARGS__, 7); break;    \
    default: f(J, __VA_ARGS__, (J)->p.mml); \
    }

#undef CAND4

/* The one table fills within the first few blocks, so for most inputs its window test is a branch
 * that guesses right; for a window under 512 KiB, an input of 256 KiB or less, filling it is much
 * of the work, and the test goes without a branch, as libzstd 1.5.7 also has it. */
static void mf_fast(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) {
    if (J->p.wlog < 19) {
        ZSE_BY_MML(mf_fast_with, J, at, end, r, 1);
    } else {
        ZSE_BY_MML(mf_fast_with, J, at, end, r, 0);
    }
}

/* Two hash tables: one of eight bytes, for long matches, and one of `mml`, for short ones; a short
 * one is taken only if the next position has no long one. The long table runs a position ahead:
 * the next position's entry is loaded before this one's candidates are compared, and it's the
 * one a short match looks at. On a miss the search moves on by a step, 1 to start with and one
 * more for each DSTEP_RUN bytes without a match. A table entry is taken only if it's in the
 * window from all of the block, so one bound serves every position. */
#ifdef ZSE_TUNE
#define DSTEP_RUN (getenv("ZSE_DS") ? (cx_usize)atoi(getenv("ZSE_DS")) : 256)
#else
#define DSTEP_RUN 256
#endif

static inline __attribute__((always_inline)) void mf_dfast_with(job_t* J, cx_u32 at, cx_u32 end, cx_u32* rp, const cx_u32 mls) {
    const cx_u8* const base = J->base;
    cx_u32* const hl8 = J->hash;
    cx_u32* const hs = J->chain;
    const cx_u32 bl = J->p.hlog, bs = J->p.clog, start = J->start;
    const cx_u32 low = end - start > J->wsize ? end - J->wsize : start;
    const cx_u8* const prefix = base + start;
    const cx_u8* const nt = not_text(base);
    cx_u32 r[3] = {rp[0], rp[1], rp[2]};
    ss_t sl = J->ss;
    ss_t* const ss = &sl;
    const cx_u8* ip = base + at;
    const cx_u8* anchor = ip;
    const cx_u8* const iend = base + end;
    const cx_u8* const ilimit = iend - READ;
    const cx_usize run = DSTEP_RUN;
    /* The repeated offset tried, as in mf_fast. */
    cx_u32 rt = REP_OK(r[0], at, start) ? r[0] : 0;
    for (;;) {
        cx_usize step = 1;
        const cx_u8* grow = ip + run;
        const cx_u8* ip1 = ip + 1;
        if (ip1 > ilimit) break;
        cx_u32 h8 = hash_at(ip, 8, bl), m8 = hl8[h8];
        cx_u32 cur, ml, off, h8n, m8n, m;
        const cx_u8* mp;
        do {
            /* ip1 = ip + step; h8 and m8 are ip's long hash and entry. */
            const cx_u32 h = hash_at(ip, mls, bs);
            m = hs[h];
            cur = (cx_u32)(ip - base);
            hl8[h8] = hs[h] = cur;
            if ((ld32(ip + 1 - rt) == ld32(ip + 1)) & (rt != 0)) {
                ml = count(ip + 5, ip + 5 - rt, iend) + 4;
                ip++;
                off = rt;
                goto found;
            }
            h8n = hash_at(ip1, 8, bl);
            if (ld64(entry_at(base, m8, low, nt)) == ld64(ip) && entry_at(base, m8, low, nt) == base + m8) {
                mp = base + m8;
                ml = count(ip + 8, mp + 8, iend) + 8;
                goto back;
            }
            m8n = hl8[h8n];
            if (ld32(entry_at(base, m, low, nt)) == ld32(ip) && entry_at(base, m, low, nt) == base + m) goto shorter;
            if (ip1 >= grow) {
                step++;
                grow += run;
            }
            ip = ip1;
            ip1 += step;
            h8 = h8n;
            m8 = m8n;
        } while (ip1 <= ilimit);
        break;
    shorter:
        if (ld64(entry_at(base, m8n, low, nt)) == ld64(ip1) && entry_at(base, m8n, low, nt) == base + m8n) {
            ip = ip1;
            mp = base + m8n;
            ml = count(ip + 8, mp + 8, iend) + 8;
        } else {
            mp = base + m;
            ml = count(ip + 4, mp + 4, iend) + 4;
        }
    back:
        while (ip > anchor && mp > prefix && ip[-1] == mp[-1]) {
            ip--;
            mp--;
            ml++;
        }
        off = (cx_u32)(ip - mp);
        /* The next position's long entry, unless the step has got so long that it could be past
         * the match's end, where the next search starts: the table mustn't hold one after that. */
        if (step < 4) hl8[h8n] = cur + (cx_u32)step;
    found:
        ss_put(ss, r, anchor, (cx_u32)(ip - anchor), off, ml);
        ip += ml;
        anchor = ip;
        if (ip <= ilimit) {
            const cx_u32 c2 = cur + 2, e = (cx_u32)(ip - base);
            hl8[hash_at(base + c2, 8, bl)] = c2;
            hl8[hash_at(ip - 2, 8, bl)] = e - 2;
            hs[hash_at(base + c2, mls, bs)] = c2;
            hs[hash_at(ip - 1, mls, bs)] = e - 1;
            for (;;) {
                const cx_u32 c3 = (cx_u32)(ip - base);
                if (ip > ilimit || !REP_OK(r[1], c3, start) || ld32(ip) != ld32(ip - r[1])) break;
                ml = count(ip + 4, ip + 4 - r[1], iend) + 4;
                hs[hash_at(ip, mls, bs)] = c3;
                hl8[hash_at(ip, 8, bl)] = c3;
                ss_put(ss, r, anchor, 0, r[1], ml);
                ip += ml;
                anchor = ip;
            }
        }
        rt = r[0];
    }
    ss_tail(ss, anchor, (cx_usize)(iend - anchor));
    J->ss = sl;
    rp[0] = r[0];
    rp[1] = r[1];
    rp[2] = r[2];
}

static void mf_dfast(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) {
    ZSE_BY_MML(mf_dfast_with, J, at, end, r);
}

/* Hash rows, for the greedy and lazy levels: each row keeps the last 2^rlog - 1 positions whose
 * hash leads to it, in slots 1 on, newest at the row's head and going round, with the next byte of
 * each one's hash beside them in a row of tags. Comparing that byte across the whole row at once
 * picks out the few positions worth reading the text at, nearly always just the right ones. Slot
 * 0 of the tags holds the head, so it comes into the cache with them. */
#define ROW_SKIP 384 /* a match longer than this leaves out all but its first 96 and last 32 positions */
#define LAZY_SKIP 8  /* a step on a miss longer than this leaves the positions skipped out */

static inline cx_u32 row_log(cx_u32 slog) { return slog < 4 ? 4 : slog > 6 ? 6 : slog; }

/* The row's positions whose byte is tag: bit k for the k-th position. A row of 16 is compared in
 * 16 bytes: 32 would reach into the next row, which a quarter of the time is in the next cache
 * line, one not fetched ahead. */
static inline cx_u64 row_tags(const cx_u8* tags, cx_u32 tag, cx_u32 rlog) {
#if defined(__AVX2__)
    if (rlog == 4) return (cx_u32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)tags), _mm_set1_epi8((char)tag)));
    const __m256i t = _mm256_set1_epi8((char)tag);
    cx_u64 m = (cx_u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)tags), t));
    if (rlog == 6) m |= (cx_u64)(cx_u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)(tags + 32)), t)) << 32;
    return m;
#elif defined(__SSE2__)
    const __m128i t = _mm_set1_epi8((char)tag);
    cx_u64 m = 0;
    for (cx_u32 k = 0; k < (1u << rlog); k += 16) m |= (cx_u64)(cx_u32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(tags + k)), t)) << k;
    return m;
#else
    cx_u64 m = 0;
    for (cx_u32 k = 0; k < (1u << rlog); k++) m |= (cx_u64)(tags[k] == tag) << k;
    return m;
#endif
}

/* What the row finder works with, copied out of the job for each block into a local the tags'
 * byte stores can't touch, so none of it is read again after each one. */
typedef struct {
    const cx_u8* base;
    cx_u32* pos;
    cx_u8* tags;
    cx_u32 rlog, rmask, hb, mls;
    cx_u32 next_up; /* the rows have every position before this */
    cx_u32 wsize, tries;
    cx_u32 hc[8]; /* the hashes of the eight positions from next_up, by position mod 8 */
    int skipping; /* in a stretch without matches: see mf_lazy */
} rw_t;

static inline void rw_load(rw_t* w, const job_t* J) {
    w->base = J->base;
    w->pos = J->hash;
    w->tags = J->tags;
    w->rlog = J->rlog;
    w->rmask = (1u << J->rlog) - 1;
    w->hb = J->p.hlog - J->rlog + 8;
    w->mls = J->p.mml;
    w->next_up = J->next_up;
    w->wsize = J->wsize;
    w->tries = 1u << J->p.slog;
    w->skipping = 0;
}

/* A position's hash: its row, and its tag in the low byte. */
static inline cx_u32 row_hash(const rw_t* w, cx_u32 i) { return hash_at(w->base + i, w->mls, w->hb); }

static inline void row_put(rw_t* w, cx_u32 i, cx_u32 h) {
    const cx_u32 at = (h >> 8) << w->rlog;
    cx_u32 hd = (w->tags[at] - 1u) & w->rmask;
    hd += hd == 0 ? w->rmask : 0;
    w->tags[at] = (cx_u8)hd;
    w->tags[at + hd] = (cx_u8)h;
    w->pos[at + hd] = i;
}

/* The next eight positions' hashes are kept, and their rows fetched into the cache, so a row is
 * there by the time its position comes to be looked up or put in. The parse stops 16 bytes before
 * the job's end, so the eight bytes hashed are always in. */
static inline void row_ahead(rw_t* w, cx_u32 i) {
    const cx_u32 h = row_hash(w, i), at = (h >> 8) << w->rlog;
    w->hc[i & 7] = h;
    __builtin_prefetch(w->pos + at);
    __builtin_prefetch(w->tags + at);
}

static inline void row_refill(rw_t* w, cx_u32 from) {
    for (cx_u32 i = from; i < from + 8; i++) row_ahead(w, i);
}

/* Puts the positions before target into their rows. Of a long match's, only the first few and the
 * last go in: the ones between would find the same text the match did. Always inlined, as row_best
 * is, for the same reason. With `safe`, each hash is worked out where it's needed, not eight ahead,
 * for a job's last bytes. */
static inline __attribute__((always_inline)) void row_insert(rw_t* w, cx_u32 target, const int safe) {
    cx_u32 i = w->next_up;
    if (target - i > ROW_SKIP) {
        for (const cx_u32 e = i + 96; i < e; i++) {
            row_put(w, i, safe ? row_hash(w, i) : w->hc[i & 7]);
            if (!safe) row_ahead(w, i + 8);
        }
        i = target - 32;
        if (!safe) row_refill(w, i);
    }
    for (; i < target; i++) {
        row_put(w, i, safe ? row_hash(w, i) : w->hc[i & 7]);
        if (!safe) row_ahead(w, i + 8);
    }
    w->next_up = target;
}

/* The longest match at ip in its row, at most 2^slog tries, newest first: its length, or 0 if it's
 * shorter than mml, and its distance in *off. Always inlined, so `w` is the caller's local, which
 * the tags' byte stores can't touch: called through a pointer, it had its fields read again after
 * each store. `safe` as for row_insert. */
static inline __attribute__((always_inline)) cx_u32 row_best(rw_t* w, const cx_u8* ip, const cx_u8* iend, cx_u32* off, const int safe) {
    const cx_u8* const base = w->base;
    const cx_u32 cur = (cx_u32)(ip - base);
    if (w->skipping) {
        w->next_up = cur;
        w->hc[cur & 7] = row_hash(w, cur);
    } else {
        row_insert(w, cur, safe);
        if (safe) w->hc[cur & 7] = row_hash(w, cur);
    }
    const cx_u32 rlog = w->rlog, rmask = w->rmask;
    const cx_u32 h = w->hc[cur & 7], at = (h >> 8) << rlog, hd = w->tags[at];
    cx_u64 m = row_tags(w->tags + at, h & 0xFF, rlog) & ~(cx_u64)1;
    if (hd) m = ((m >> hd) | (m << ((1u << rlog) - hd))) & (rlog == 6 ? ~(cx_u64)0 : ((cx_u64)1 << (1u << rlog)) - 1);
    const cx_u32* const rp = w->pos + at;
    const cx_u32 low = cur - w->wsize;
    cx_u32 best = w->mls - 1, best_off = 0;
    const cx_u32 room = (cx_u32)(iend - ip);
    /* The candidates first, each one's text fetched as it's found, so the fetches wait together
     * rather than one after another; then they're compared. */
    cx_u32 cands[64], nc = 0;
    for (const cx_u32 tries = w->tries; m && nc < tries; m &= m - 1) {
        const cx_u32 cand = rp[((cx_u32)__builtin_ctzll(m) + hd) & rmask];
        if (cand < low) break;
        __builtin_prefetch(base + cand);
        cands[nc++] = cand;
    }
    /* This position goes into its row now, while the row is at hand, rather than with the next
     * search's. The candidates are taken already, so this search doesn't find it. */
    row_put(w, cur, h);
    w->next_up = cur + 1;
    if (!safe && !w->skipping) row_ahead(w, cur + 8);
    for (cx_u32 k = 0; k < nc; k++) {
        const cx_u32 cand = cands[k];
        const cx_u8* mp = base + cand;
        /* Only a match longer than the best has the four bytes up to the best's length, so one
         * compare of them, and one branch, passes all the candidates worth counting. */
        if (ld32(mp + best - 3) == ld32(ip + best - 3)) {
            const cx_u32 len = count(ip, mp, iend);
            /* Further back costs more bits: a longer match has to make up for them. */
            if (len > best && (!best_off || (int)(len - best) * 4 > (int)high_bit(cur - cand + 3) - (int)high_bit(best_off + 3))) {
                best = len;
                best_off = cur - cand;
                if (len >= room) break;
            }
        }
    }
    *off = best_off;
    return best_off ? best : 0;
}

/* The parse for the hash-row levels: the longest match at a position, unless the next one or,
 * for lazy2, the one after has one that's worth more by its length and offset. */
static inline __attribute__((always_inline)) void mf_lazy_with(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r, const int depth, const cx_u32 mls) {
    const cx_u8* const base = J->base;
    const cx_u32 start = J->start;
    ss_t* const ss = &J->ss;
    const cx_u8* ip = base + at;
    const cx_u8* anchor = ip;
    const cx_u8* const iend = base + end;
    /* Positions are hashed eight ahead of the one looked at, eight bytes each: the job's last 16
     * get the plainer search after this loop, so that stays in its text. */
    const cx_u8* const ilimit = end - READ <= J->to - 16 ? iend - READ : base + J->to - 16;
    if (J->next_up < start) J->next_up = start;
    if (iend - READ <= ip + 1) goto tail;
    rw_t w;
    rw_load(&w, J);
    w.mls = mls;
    if (ilimit > ip + 1) row_refill(&w, w.next_up);
    while (ip < ilimit) {
        cx_u32 ml = 0, off = 0;
        const cx_u8* mstart = ip;
        {
            const cx_u32 c1 = (cx_u32)(ip + 1 - base);
            if (REP_OK(r[0], c1, start) && ld32(ip + 1) == ld32(ip + 1 - r[0])) {
                ml = count(ip + 5, ip + 5 - r[0], iend) + 4;
                off = r[0];
                mstart = ip + 1;
                if (depth == 0) goto store;
            }
        }
        {
            cx_u32 o;
            const cx_u32 l = row_best(&w, ip, iend, &o, 0);
            if (l > ml) {
                ml = l;
                off = o;
                mstart = ip;
            }
        }
        if (ml == 0) {
            /* On a miss it moves on further the longer it's been since the last match. Once that's
             * more than LAZY_SKIP a time, the positions it moves over stop going into the rows,
             * just the ones it looks at: a stretch that doesn't compress costs little more than
             * the looking, as in libzstd 1.5.7, for a little less found in it later. The rows
             * take every position again from the next match. */
            const cx_usize step = ((cx_usize)(ip - anchor) >> 8) + 1;
            ip += step;
            w.skipping = step > LAZY_SKIP;
            continue;
        }
        /* Whether a match a byte or two on is worth more: its length, less the bits its offset
         * takes, against the one in hand's with a bonus for being sooner. Each better one found
         * starts the looking ahead over from there. */
        {
            int d = 0;
            while (d < depth && ip < ilimit) {
                ip++;
                d++;
                const cx_u32 c = (cx_u32)(ip - base);
                const int bonus = d == 1 ? 4 : 7;
                int worth = (int)ml * 4 - (int)high_bit(off == r[0] ? 1 : off + 3) + bonus;
                int better = 0;
                if (REP_OK(r[0], c, start) && ld32(ip) == ld32(ip - r[0])) {
                    const cx_u32 l = count(ip + 4, ip + 4 - r[0], iend) + 4;
                    if ((int)l * 4 > worth) {
                        ml = l;
                        off = r[0];
                        mstart = ip;
                        worth = (int)l * 4;
                        better = 1;
                    }
                }
                cx_u32 o;
                const cx_u32 l = row_best(&w, ip, iend, &o, 0);
                if (l && (int)l * 4 - (int)high_bit(o + 3) > worth) {
                    ml = l;
                    off = o;
                    mstart = ip;
                    better = 1;
                }
                if (better) d = 0;
            }
        }
        /* A new offset reaches back as far as the bytes before it match. */
        if (off != r[0]) {
            const cx_u8* mp = mstart - off;
            while (mstart > anchor && mp > base + start && mstart[-1] == mp[-1]) {
                mstart--;
                mp--;
                ml++;
            }
        }
    store:
        if (w.skipping) {
            w.skipping = 0;
            row_refill(&w, w.next_up);
        }
        ss_put(ss, r, anchor, (cx_u32)(mstart - anchor), off, ml);
        ip = mstart + ml;
        anchor = ip;
        for (;;) {
            const cx_u32 c3 = (cx_u32)(ip - base);
            if (ip >= ilimit || !REP_OK(r[1], c3, start) || ld32(ip) != ld32(ip - r[1])) break;
            ml = count(ip + 4, ip + 4 - r[1], iend) + 4;
            ss_put(ss, r, anchor, 0, r[1], ml);
            ip += ml;
            anchor = ip;
        }
    }
    /* A job's last bytes, to READ before its end: the first match at each, the repeated offset's
     * or the row's, with no hashing ahead. A small input's end, which a dictionary often has, is a
     * good part of it. */
    for (const cx_u8* const tlimit = iend - READ; ip < tlimit;) {
        cx_u32 ml = 0, off = 0;
        const cx_u8* mstart = ip;
        const cx_u32 c1 = (cx_u32)(ip + 1 - base);
        if (REP_OK(r[0], c1, start) && ld32(ip + 1) == ld32(ip + 1 - r[0])) {
            ml = count(ip + 5, ip + 5 - r[0], iend) + 4;
            off = r[0];
            mstart = ip + 1;
        }
        cx_u32 o;
        const cx_u32 l = row_best(&w, ip, iend, &o, 1);
        if (l > ml) {
            ml = l;
            off = o;
            mstart = ip;
        }
        if (ml == 0) {
            ip++;
            continue;
        }
        if (off != r[0]) {
            const cx_u8* mp = mstart - off;
            while (mstart > anchor && mp > base + start && mstart[-1] == mp[-1]) {
                mstart--;
                mp--;
                ml++;
            }
        }
        ss_put(ss, r, anchor, (cx_u32)(mstart - anchor), off, ml);
        ip = mstart + ml;
        anchor = ip;
    }
    J->next_up = w.next_up;
tail:
    ss_tail(ss, anchor, (cx_usize)(iend - anchor));
}

/* A copy of the parse for each depth, and for the shortest match of 5 the row levels have, so the
 * looking ahead and the hash's length are worked out when it's compiled. */
#define ZSE_LAZY(J, at, end, r, depth) \
    if ((J)->p.mml == 5) mf_lazy_with(J, at, end, r, depth, 5); \
    else mf_lazy_with(J, at, end, r, depth, (J)->p.mml)
static void mf_greedy(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) { ZSE_LAZY(J, at, end, r, 0); }
static void mf_lazy1(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) { ZSE_LAZY(J, at, end, r, 1); }
static void mf_lazy2(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) { ZSE_LAZY(J, at, end, r, 2); }

/* Binary trees: the positions with the same hash of their first bytes, kept sorted by the text that
 * follows them, two links a position, so a search goes down one path and each step agrees with
 * the text for at least as long as the step before. The tree holds the last 2^(clog-1)
 * positions. At the levels whose shortest match is 3, a second table holds the last position for
 * each hash of three bytes. */
#define HASH3_LOG 16
/* A three-byte match further back than this costs more than its literals. */
#define HASH3_REACH ((cx_u32)1 << 16)

/* How far the trees compare two positions' text: past it, they're taken as the same. */
#define BT_CMP ((cx_u32)1 << 16)

/* Puts cur, at ip, into its tree, noting each match longer than `best` in m, longest last, each cut
 * to `room`, the bytes left in its block: gives how many. *longest is the longest agreement met.
 *
 * The comparisons go to the job's end, or BT_CMP bytes, the same for every position however near
 * a block's end it is: a tree stays sorted only if each place in it was found by comparing as far
 * as the others were. bt_place does it for the tree below cand, whose positions before low have
 * left it: their places in it may be newer positions' by now.
 *
 * A dictionary's content goes in with the comparisons stopping at its end, J->lim, so the trees
 * are the same whatever text follows it, and kept ones can be used again (digest_take). Text that
 * agrees to there can't be put in order, so, as libzstd does, cur goes in with nothing below it. */
static inline __attribute__((always_inline)) cx_u32 bt_place_with(job_t* J, const cx_u8* ip, cx_u32 cur, cx_u32 cand, cx_u32 low, cx_u32 room, cx_u32 best, match_t* m,
                                                                  cx_u32* longest, cx_u32 depth, const int undo) {
    const cx_u32 left = J->lim - cur;
    const cx_u32 limit = left < BT_CMP ? left : BT_CMP;
    const cx_u32 mask = (1u << (J->p.clog - 1)) - 1;
    cx_u32* const son = J->chain;
    cx_u32* ptr0 = son + 2 * (cur & mask) + 1;
    cx_u32* ptr1 = son + 2 * (cur & mask);
    /* The positions whose nodes ptr0 and ptr1 are in, and the log, in locals: through J they'd be
     * read again after each store into the trees. */
    cx_u32 own0 = cur, own1 = cur;
    cx_u32* const ulog = J->ulog;
    const cx_u32 ulo = J->ulo, ucap = J->ulog_cap;
    cx_u32 un = J->ulog_n;
#define BT_W(ptr, own, v)                                   \
    do {                                                    \
        if (undo && (own) < ulo) {                          \
            if (un < ucap) {                                \
                ulog[2 * un] = (cx_u32)((ptr) - son);       \
                ulog[2 * un + 1] = *(ptr);                  \
                un++;                                       \
            } else {                                        \
                un = ucap + 1;                              \
            }                                               \
        }                                                   \
        *(ptr) = (v);                                       \
    } while (0)
    cx_u32 len0 = 0, len1 = 0, noted = 0, top = 0;
    for (;;) {
        const cx_u32 delta = cur - cand;
        if (depth-- == 0 || cand < low) {
            BT_W(ptr1, own1, 0);
            BT_W(ptr0, own0, 0);
            break;
        }
        cx_u32* const pair = son + 2 * (cand & mask);
        const cx_u8* const pb = ip - delta;
        cx_u32 len = len0 < len1 ? len0 : len1;
        /* Whichever way the text sends the search, it goes to one of these two next: both their
         * links and their text are fetched while this one is compared. */
        __builtin_prefetch(son + 2 * (pair[0] & mask));
        __builtin_prefetch(son + 2 * (pair[1] & mask));
        __builtin_prefetch(ip - (cur - pair[0]) + len);
        __builtin_prefetch(ip - (cur - pair[1]) + len);
        if (pb[len] == ip[len]) {
            len += 1 + count(ip + len + 1, pb + len + 1, ip + limit);
            if (len > top) top = len;
            cx_u32 got = len < room ? len : room;
            if (len == BT_CMP && room > BT_CMP) got = BT_CMP + count(ip + BT_CMP, pb + BT_CMP, ip + room);
            if (got > best) {
                best = got;
                m[noted].len = got;
                m[noted].off = delta;
                noted++;
            }
            if (len >= limit) {
                if (J->cut) {
                    BT_W(ptr1, own1, 0);
                    BT_W(ptr0, own0, 0);
                    break;
                }
                BT_W(ptr1, own1, pair[0]);
                BT_W(ptr0, own0, pair[1]);
                break;
            }
        }
        if (pb[len] < ip[len]) {
            BT_W(ptr1, own1, cand);
            ptr1 = pair + 1;
            own1 = cand;
            cand = *ptr1;
            len1 = len;
        } else {
            BT_W(ptr0, own0, cand);
            ptr0 = pair;
            own0 = cand;
            cand = *ptr0;
            len0 = len;
        }
    }
#undef BT_W
    if (undo) J->ulog_n = un;
    *longest = top;
    return noted;
}

static cx_u32 bt_place(job_t* J, const cx_u8* ip, cx_u32 cur, cx_u32 cand, cx_u32 low, cx_u32 room, cx_u32 best, match_t* m, cx_u32* longest, cx_u32 depth) {
    if (J->ulo) return bt_place_with(J, ip, cur, cand, low, room, best, m, longest, depth, 1);
    return bt_place_with(J, ip, cur, cand, low, room, best, m, longest, depth, 0);
}

#define BT_AHEAD 8

static cx_u32 bt_insert(job_t* J, const cx_u8* ip, cx_u32 cur, cx_u32 room, cx_u32 best, match_t* m, cx_u32* longest, cx_u32 depth) {
    const cx_u32 h = hash_at(ip, J->bt_mls, J->p.hlog);
    const cx_u32 cand = J->hash[h];
    J->hash[h] = cur;
    const cx_u32 mask = (1u << (J->p.clog - 1)) - 1;
    /* The next positions go in next, nearly always: the table's entry for one a few on, and for
     * the next one its tree's root's links and text, are fetched while this one goes down its own. */
    if (cur + BT_AHEAD + READ <= J->to) __builtin_prefetch(J->hash + hash_at(ip + BT_AHEAD, J->bt_mls, J->p.hlog));
    if (cur + 1 + READ <= J->to) {
        const cx_u32 c1 = J->hash[hash_at(ip + 1, J->bt_mls, J->p.hlog)];
        __builtin_prefetch(J->chain + 2 * (c1 & mask));
        __builtin_prefetch(J->base + c1);
    }
    return bt_place(J, ip, cur, cand, cur - (mask < J->wsize ? mask : J->wsize), room, best, m, longest, depth);
}

/* For btlazy2, whose matches pass over most positions without looking for one there, a position
 * first goes on its hash's list unsorted: its first link to the one before, its second
 * DT_UNSORTED. A search puts the newest of its hash's unsorted positions into the tree, oldest
 * first, before it goes down it; past 2^slog of them, the older ones are let go. */
#define DT_UNSORTED 1u
#define DT_MAX 64
#define DT_AHEAD 16

static void dt_update(job_t* J, cx_u32 target) {
    const cx_u8* const base = J->base;
    const cx_u32 mask = (1u << (J->p.clog - 1)) - 1, end = J->to - READ;
    cx_u32* const son = J->chain;
    for (cx_u32 i = J->next_up; i < target && i <= end; i++) {
        /* The table's entry for a position some way on is fetched now, as each is a miss. */
        if (i + DT_AHEAD <= end) __builtin_prefetch(J->hash + hash_at(base + i + DT_AHEAD, J->bt_mls, J->p.hlog));
        const cx_u32 h = hash_at(base + i, J->bt_mls, J->p.hlog);
        cx_u32* const pair = son + 2 * (i & mask);
        pair[0] = J->hash[h];
        pair[1] = DT_UNSORTED;
        J->hash[h] = i;
    }
    if (target > J->next_up) J->next_up = target;
}

/* The longest match at ip from its tree, which ip goes into after the unsorted positions before
 * it: its length, or 0 if it's shorter than mml, and its distance in *off. */
static inline cx_u32 dt_best(job_t* J, const cx_u8* ip, const cx_u8* iend, cx_u32* off) {
    const cx_u8* const base = J->base;
    const cx_u32 cur = (cx_u32)(ip - base);
    dt_update(J, cur);
    const cx_u32 mask = (1u << (J->p.clog - 1)) - 1;
    const cx_u32 reach = mask < J->wsize ? mask : J->wsize;
    const cx_u32 depth = 1u << J->p.slog, most = depth < DT_MAX ? depth : DT_MAX;
    cx_u32* const son = J->chain;
    const cx_u32 h = hash_at(ip, J->bt_mls, J->p.hlog);
    cx_u32 got[DT_MAX], n = 0;
    const cx_u32 low = cur - reach;
    for (cx_u32 c = J->hash[h]; c >= low && son[2 * (c & mask) + 1] == DT_UNSORTED; c = son[2 * (c & mask)]) {
        if (n == most) {
            son[2 * (c & mask)] = son[2 * (c & mask) + 1] = 0;
            break;
        }
        got[n++] = c;
    }
    while (n--) {
        const cx_u32 x = got[n];
        cx_u32 top;
        (void)bt_place(J, base + x, x, son[2 * (x & mask)], low, 0, ~0u, 0, &top, depth);
    }
    match_t* const m = J->mt;
    cx_u32 top;
    const cx_u32 k = bt_insert(J, ip, cur, (cx_u32)(iend - ip), J->p.mml - 1, m, &top, depth);
    J->next_up = cur + 1;
    if (!k) return 0;
    /* Of the matches met, each longer than the last and mostly further back, the one worth most
     * by the lazy parse's measure: a longer one has to make up for its offset's bits. */
    cx_u32 b = 0;
    int worth = (int)m[0].len * 4 - (int)high_bit(m[0].off + 3);
    for (cx_u32 i = 1; i < k; i++) {
        const int w = (int)m[i].len * 4 - (int)high_bit(m[i].off + 3);
        if (w > worth) {
            worth = w;
            b = i;
        }
    }
    *off = m[b].off;
    return m[b].len;
}

/* Brings the trees up to position target: each position before it put in, but those inside a long
 * repeat, which would each compare all of it, are passed over. */
static void bt_update(job_t* J, cx_u32 target, const cx_u8* iend) {
    const cx_u8* const base = J->base;
    cx_u32 i = J->next_up;
    while (i < target) {
        const cx_u8* ip = base + i;
        const cx_u32 room = (cx_u32)(iend - ip);
        if (J->to - i < READ) {
            i++;
            continue;
        }
        if (J->hash3) J->hash3[hash_at(ip, 3, HASH3_LOG)] = i;
        cx_u32 top;
        (void)bt_insert(J, ip, i, room, ~0u, 0, &top, J->bt_up);
        i += top > 384 ? top - 384 : 1;
    }
    if (i > J->next_up) J->next_up = i;
}

/* The match finder's view of offset `off` after a run of literals, ll0 if there were none: its
 * code, without moving the repeated offsets on. */
static inline cx_u32 ofb_of(const cx_u32* r, int ll0, cx_u32 off) {
    if (!ll0) {
        if (off == r[0]) return 1;
        if (off == r[1]) return 2;
        if (off == r[2]) return 3;
    } else {
        if (off == r[1]) return 1;
        if (off == r[2]) return 2;
        if (off == r[0] - 1) return 3;
    }
    return off + 3;
}

/* The repeated offsets after offset `off`, as ofb_of codes it. */
static inline void rep_after(cx_u32* out, const cx_u32* r, int ll0, cx_u32 off) {
    const cx_u32 ofb = ofb_of(r, ll0, off);
    if (ofb == 1 && !ll0) {
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
    } else if ((ofb == 2 && !ll0) || (ofb == 1 && ll0)) {
        out[0] = off;
        out[1] = r[0];
        out[2] = r[2];
    } else {
        out[0] = off;
        out[1] = r[0];
        out[2] = r[1];
    }
}

/* Every match at ip worth noting, shortest first, each longer than the one before: the repeated
 * offsets' (r, after a run of literals unless ll0), the nearest three-byte one, and the trees'.
 * ip goes into the trees. Gives how many, into m. */
static cx_u32 all_matches(job_t* J, const cx_u8* ip, const cx_u8* iend, const cx_u32* r, int ll0, match_t* m) {
    const cx_u8* const base = J->base;
    const cx_u32 cur = (cx_u32)(ip - base), start = J->start, mml = J->p.mml;
    bt_update(J, cur, iend);
    const cx_u32 room = (cx_u32)(iend - ip);
    /* A repeated offset's match pays from 4 bytes, or 3 where the level's matches do. */
    cx_u32 n = 0, best = (mml == 3 ? 3 : 4) - 1;
    for (int k = 0; k < 3; k++) {
        const cx_u32 d = ll0 ? (k == 0 ? r[1] : k == 1 ? r[2] : r[0] - 1) : r[k];
        if (!REP_OK(d, cur, start) || d > J->wsize) continue;
        const cx_u8* mp = ip - d;
        if (mp[0] != ip[0] || mp[1] != ip[1] || mp[2] != ip[2]) continue;
        const cx_u32 len = 3 + count(ip + 3, mp + 3, iend);
        if (len > best) {
            m[n].len = len;
            m[n].off = d;
            n++;
            best = len;
        }
    }
    if (J->hash3) {
        const cx_u32 h3 = hash_at(ip, 3, HASH3_LOG);
        const cx_u32 c3 = J->hash3[h3];
        J->hash3[h3] = cur;
        if (best < 3 && cur - c3 <= HASH3_REACH && c3 >= start) {
            const cx_u8* mp = base + c3;
            if (mp[0] == ip[0] && mp[1] == ip[1] && mp[2] == ip[2]) {
                const cx_u32 len = 3 + count(ip + 3, mp + 3, iend);
                m[n].len = len;
                m[n].off = cur - c3;
                n++;
                best = len;
            }
        }
    }
    cx_u32 top;
    n += bt_insert(J, ip, cur, room, best > mml - 1 ? best : mml - 1, m + n, &top, 1u << J->p.slog);
    J->next_up = cur + 1;
    return n;
}

/* --- the optimal parser ---------------------------------------------------------------------- */

#define INF_PRICE (1u << 30)

static void prices_set(stats_t* s) {
    const cx_u32 ls = lg8(s->lit_sum), lls = lg8(s->ll_sum), mls = lg8(s->ml_sum), ofs = lg8(s->of_sum);
    for (int i = 0; i < 256; i++) {
        cx_u32 p = ls - lg8(s->lit[i]);
        if (p > 11 * 256) p = 11 * 256;
        if (p < 256) p = 256; /* a Huffman code is a bit at least */
        s->lit_p[i] = p;
    }
    for (int i = 0; i <= LL_MAX; i++) s->ll_p[i] = lls - lg8(s->ll[i]) + LL_EXTRA[i] * 256;
    for (int i = 0; i <= ML_MAX; i++) s->ml_p[i] = mls - lg8(s->ml[i]) + ML_EXTRA[i] * 256;
    for (int i = 0; i <= OF_MAX; i++) s->of_p[i] = ofs - lg8(s->of[i]) + (cx_u32)i * 256;
}

static cx_u32 sum_of(const cx_u32* f, int n) {
    cx_u32 s = 0;
    for (int i = 0; i < n; i++) s += f[i];
    return s;
}

/* At a block's start: the first block's counts from its bytes and the predefined distributions,
 * the others' halved. */
static void stats_start(stats_t* s, const cx_u8* src, cx_u32 n) {
    if (!s->ready) {
        cx_u32 c[256] = {0};
        for (cx_u32 i = 0; i < n; i++) c[src[i]]++;
        for (int i = 0; i < 256; i++) s->lit[i] = 1 + (c[i] >> 4);
        for (int i = 0; i <= LL_MAX; i++) s->ll[i] = LL_NORM[i] > 0 ? (cx_u32)LL_NORM[i] : 1;
        for (int i = 0; i <= ML_MAX; i++) s->ml[i] = ML_NORM[i] > 0 ? (cx_u32)ML_NORM[i] : 1;
        for (int i = 0; i <= OF_MAX; i++) s->of[i] = i <= 28 && OF_NORM[i] > 0 ? (cx_u32)OF_NORM[i] : 1;
        s->ready = 1;
    } else {
        for (int i = 0; i < 256; i++) s->lit[i] = 1 + (s->lit[i] >> 1);
        for (int i = 0; i <= LL_MAX; i++) s->ll[i] = 1 + (s->ll[i] >> 1);
        for (int i = 0; i <= ML_MAX; i++) s->ml[i] = 1 + (s->ml[i] >> 1);
        for (int i = 0; i <= OF_MAX; i++) s->of[i] = 1 + (s->of[i] >> 1);
    }
    s->lit_sum = sum_of(s->lit, 256);
    s->ll_sum = sum_of(s->ll, LL_MAX + 1);
    s->ml_sum = sum_of(s->ml, ML_MAX + 1);
    s->of_sum = sum_of(s->of, OF_MAX + 1);
    prices_set(s);
}

/* A sequence taken: its literals and codes counted. */
static void stats_add(stats_t* s, const cx_u8* lit, cx_u32 ll, cx_u32 ofb, cx_u32 ml) {
    for (cx_u32 i = 0; i < ll; i++) s->lit[lit[i]] += 2;
    s->lit_sum += 2 * ll;
    s->ll[ll_code(ll)]++;
    s->ll_sum++;
    s->ml[ml_code(ml - 3)]++;
    s->ml_sum++;
    s->of[high_bit(ofb)]++;
    s->of_sum++;
}

static inline cx_u32 ll_price(const stats_t* s, cx_u32 ll) { return s->ll_p[ll_code(ll)]; }

static inline cx_u32 match_price(const stats_t* s, cx_u32 ofb, cx_u32 ml) { return s->of_p[high_bit(ofb)] + s->ml_p[ml_code(ml - 3)]; }

/* Parses base[at..end) by prices: from each place, every match and the literal, each priced from
 * the counts, the cheapest way to each place kept, up to OPT_NUM bytes on or a match that's long
 * enough; then the way back from the furthest place is taken. */
static void mf_opt(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) {
    const cx_u8* const base = J->base;
    ss_t* const ss = &J->ss;
    stats_t* const st = J->st;
    node_t* const opt = J->opt;
    match_t* const mt = J->mt;
    const cx_u8* ip = base + at;
    const cx_u8* anchor = ip;
    const cx_u8* const iend = base + end;
    const cx_u8* const ilimit = iend - READ;
    const cx_u32 mml = J->p.mml;
    const cx_u32 enough = J->p.tlen < OPT_NUM ? J->p.tlen : OPT_NUM - 1;
    if (J->next_up < J->start) J->next_up = J->start;
    stats_start(st, base + at, end - at);
    if (end - at <= READ + 1) goto tail;
    while (ip < ilimit) {
        const cx_u32 lit0 = (cx_u32)(ip - anchor);
        cx_u32 nm = all_matches(J, ip, iend, r, lit0 == 0, mt);
        if (!nm) {
            ip++;
            continue;
        }
        cx_u32 last = mt[nm - 1].len;
        if (last >= enough) {
            /* Long enough to take as it is. */
            const cx_u32 off = mt[nm - 1].off;
            const cx_u32 ofb = ofb_of(r, lit0 == 0, off);
            ss_put(ss, r, anchor, lit0, off, last);
            stats_add(st, anchor, lit0, ofb, last);
            ip += last;
            anchor = ip;
            continue;
        }
        opt[0].price = ll_price(st, lit0);
        opt[0].off = 0;
        opt[0].mlen = 0;
        opt[0].litlen = lit0;
        opt[0].rep[0] = r[0];
        opt[0].rep[1] = r[1];
        opt[0].rep[2] = r[2];
        for (cx_u32 p = 1; p <= last; p++) opt[p].price = INF_PRICE;
        {
            cx_u32 l = mml < 4 ? mml : 4;
            for (cx_u32 k = 0; k < nm; k++) {
                const cx_u32 off = mt[k].off;
                const cx_u32 ofb = ofb_of(r, lit0 == 0, off);
                const cx_u32 base_price = opt[0].price + st->of_p[high_bit(ofb)] + ll_price(st, 0);
                for (; l <= mt[k].len; l++) {
                    node_t* q = &opt[l];
                    q->price = base_price + st->ml_p[ml_code(l - 3)];
                    q->off = off;
                    q->mlen = l;
                    q->litlen = 0;
                    rep_after(q->rep, r, lit0 == 0, off);
                }
            }
        }
        cx_u32 cur;
        cx_u32 taken = 0; /* a match long enough to end the parse at, from cur */
        for (cur = 1; cur <= last; cur++) {
            const node_t* pv = &opt[cur - 1];
            node_t* q = &opt[cur];
            {
                const cx_u32 L = pv->litlen + 1;
                const cx_u32 lp = pv->price + st->lit_p[ip[cur - 1]] + ll_price(st, L) - ll_price(st, L - 1);
                if (lp <= q->price) {
                    q->price = lp;
                    q->off = 0;
                    q->mlen = 0;
                    q->litlen = L;
                    q->rep[0] = pv->rep[0];
                    q->rep[1] = pv->rep[1];
                    q->rep[2] = pv->rep[2];
                }
            }
            if (cur == last) break;
            const cx_u8* const here = ip + cur;
            if (here >= ilimit) continue;
            /* No match from here if the next place is cheaper already: it's put in the trees
             * all the same when the next search brings them up to it. */
            if (opt[cur + 1].price <= q->price) continue;
            const int ll0 = q->litlen == 0;
            nm = all_matches(J, here, iend, q->rep, ll0, mt);
            if (!nm) continue;
            const cx_u32 top = mt[nm - 1].len;
            if (top >= enough || cur + top >= OPT_NUM) {
                taken = top;
                break;
            }
            const cx_u32 lp0 = q->price + ll_price(st, 0);
            cx_u32 l = mml < 4 ? mml : 4;
            for (cx_u32 k = 0; k < nm; k++) {
                const cx_u32 off = mt[k].off;
                const cx_u32 ofb = ofb_of(q->rep, ll0, off);
                const cx_u32 base_price = lp0 + st->of_p[high_bit(ofb)];
                for (; l <= mt[k].len; l++) {
                    const cx_u32 pos = cur + l;
                    while (last < pos) opt[++last].price = INF_PRICE;
                    const cx_u32 price = base_price + st->ml_p[ml_code(l - 3)];
                    node_t* t = &opt[pos];
                    if (price < t->price) {
                        t->price = price;
                        t->off = off;
                        t->mlen = l;
                        t->litlen = 0;
                        rep_after(t->rep, q->rep, ll0, off);
                    }
                }
            }
        }
        /* The way back: to cur, then the long match from there, or the cheapest way to last. */
        const cx_u32 end_pos = taken ? cur : last;
        const cx_u32 taken_off = taken ? mt[nm - 1].off : 0;
        /* The matches on the way, in order: their starts and ends noted backwards first. */
        cx_u32 nseq = 0;
        {
            cx_u32 pos = end_pos;
            while (pos > 0) {
                const node_t* t = &opt[pos];
                if (t->mlen) {
                    J->path[nseq].len = t->mlen;
                    J->path[nseq].off = t->off;
                    nseq++;
                    pos -= t->mlen;
                    J->path_at[nseq - 1] = pos;
                } else {
                    pos--;
                }
            }
        }
        for (cx_u32 k = nseq; k-- > 0;) {
            const cx_u8* mstart = ip + J->path_at[k];
            const cx_u32 ll = (cx_u32)(mstart - anchor), off = J->path[k].off, ml = J->path[k].len;
            const cx_u32 ofb = ofb_of(r, ll == 0, off);
            ss_put(ss, r, anchor, ll, off, ml);
            stats_add(st, anchor, ll, ofb, ml);
            anchor = mstart + ml;
        }
        if (taken) {
            const cx_u8* mstart = ip + end_pos;
            const cx_u32 ll = (cx_u32)(mstart - anchor);
            const cx_u32 ofb = ofb_of(r, ll == 0, taken_off);
            ss_put(ss, r, anchor, ll, taken_off, taken);
            stats_add(st, anchor, ll, ofb, taken);
            anchor = mstart + taken;
        }
        ip += end_pos + taken;
        prices_set(st);
    }
tail:
    ss_tail(ss, anchor, (cx_usize)(iend - anchor));
}

/* The lazy parse with the trees, two places ahead, for btlazy2. */
static void mf_btlazy(job_t* J, cx_u32 at, cx_u32 end, cx_u32* r) {
    const cx_u8* const base = J->base;
    const cx_u32 start = J->start;
    ss_t* const ss = &J->ss;
    const cx_u8* ip = base + at;
    const cx_u8* anchor = ip;
    const cx_u8* const iend = base + end;
    const cx_u8* const ilimit = iend - READ;
    if (J->next_up < start) J->next_up = start;
    if (end - at <= READ + 1) goto tail;
    while (ip < ilimit) {
        cx_u32 ml = 0, off = 0;
        const cx_u8* mstart = ip;
        {
            const cx_u32 c1 = (cx_u32)(ip + 1 - base);
            if (REP_OK(r[0], c1, start) && ld32(ip + 1) == ld32(ip + 1 - r[0])) {
                ml = count(ip + 5, ip + 5 - r[0], iend) + 4;
                off = r[0];
                mstart = ip + 1;
            }
        }
        {
            cx_u32 o;
            const cx_u32 l = dt_best(J, ip, iend, &o);
            if (l > ml) {
                ml = l;
                off = o;
                mstart = ip;
            }
        }
        if (ml == 0) {
            ip += ((cx_usize)(ip - anchor) >> 8) + 1;
            continue;
        }
        {
            int d = 0;
            while (d < 2 && ip < ilimit) {
                ip++;
                d++;
                const cx_u32 c = (cx_u32)(ip - base);
                const int bonus = d == 1 ? 4 : 7;
                const int hb = (int)high_bit(off == r[0] ? 1 : off + 3);
                int worth = (int)ml * 4 - hb + bonus;
                int better = 0;
                if (REP_OK(r[0], c, start) && ld32(ip) == ld32(ip - r[0])) {
                    const cx_u32 l = count(ip + 4, ip + 4 - r[0], iend) + 4;
                    if ((int)l * 4 > (int)ml * 4 - hb + bonus) {
                        ml = l;
                        off = r[0];
                        mstart = ip;
                        worth = (int)l * 4;
                        better = 1;
                    }
                }
                cx_u32 o;
                const cx_u32 l = dt_best(J, ip, iend, &o);
                if (l && (int)l * 4 - (int)high_bit(o + 3) > worth) {
                    ml = l;
                    off = o;
                    mstart = ip;
                    better = 1;
                }
                if (better) d = 0;
            }
        }
        if (off != r[0]) {
            const cx_u8* mp = mstart - off;
            while (mstart > anchor && mp > base + start && mstart[-1] == mp[-1]) {
                mstart--;
                mp--;
                ml++;
            }
        }
        ss_put(ss, r, anchor, (cx_u32)(mstart - anchor), off, ml);
        ip = mstart + ml;
        anchor = ip;
        for (;;) {
            const cx_u32 c3 = (cx_u32)(ip - base);
            if (ip >= ilimit || !REP_OK(r[1], c3, start) || ld32(ip) != ld32(ip - r[1])) break;
            ml = count(ip + 4, ip + 4 - r[1], iend) + 4;
            ss_put(ss, r, anchor, 0, r[1], ml);
            ip += ml;
            anchor = ip;
        }
    }
tail:
    ss_tail(ss, anchor, (cx_usize)(iend - anchor));
}

/* --- blocks ---------------------------------------------------------------------------------- */

/* The counts the block splitter keeps for a stretch of a block, which the sections then use: the
 * literals', then the three codes'. */
#define SPLIT_LL 256
#define SPLIT_ML (SPLIT_LL + LL_MAX + 1)
#define SPLIT_OF (SPLIT_ML + ML_MAX + 1)
#define SPLIT_W (SPLIT_OF + 32)

/* One past the last of counts c[0..k) in use. */
static cx_u32 used_to(const cx_u32* c, cx_u32 k) {
    while (k && !c[k - 1]) k--;
    return k;
}

/* The literals' section for lit[0..n): raw, one byte repeated, or Huffman-coded with a new tree or
 * the last block's, whichever is smallest. known is their counts, if the splitter has them. Gives
 * its size, or 0 if it doesn't fit. */
static cx_usize lit_section(const ent_t* prev, ent_t* next, const cx_u8* lit, cx_usize n, int huf_allowed, const cx_u32* known, cx_u8* out, cx_usize cap, cx_u8* tmp) {
    const cx_usize raw_head = n < 32 ? 1 : n < 4096 ? 2 : 3;
    next->huf_ok = prev->huf_ok;
    next->huf_log = prev->huf_log;
    if (prev->huf_ok) memcpy(next->huf, prev->huf, sizeof next->huf);
    cx_u32 count[256] = {0};
    if (known) {
        memcpy(count, known, sizeof count);
    } else if (n >= 2) {
        cx_u32 c1[256] = {0};
        cx_usize i = 0;
        for (; i + 1 < n; i += 2) {
            count[lit[i]]++;
            c1[lit[i + 1]]++;
        }
        if (i < n) count[lit[i]]++;
        for (int s = 0; s < 256; s++) count[s] += c1[s];
    } else if (n == 1) {
        count[lit[0]]++;
    }
    cx_u32 max = 0, distinct = 0;
    for (cx_u32 s = 0; s < 256; s++) {
        if (count[s]) {
            max = s;
            distinct++;
        }
    }
    if (n > 0 && distinct == 1 && n >= 3) {
        /* One byte, repeated. */
        if (cap < raw_head + 1) return 0;
        const cx_u32 h = 1 | (n < 32 ? (cx_u32)n << 3 : n < 4096 ? (1u << 2) | ((cx_u32)n << 4) : (3u << 2) | ((cx_u32)n << 4));
        if (raw_head == 1) out[0] = (cx_u8)h;
        else if (raw_head == 2) st16(out, h);
        else st24(out, h);
        out[raw_head] = lit[0];
        return raw_head + 1;
    }
    if (huf_allowed && n >= HUF_MIN && distinct >= 2) {
        cx_u8 len[256];
        const cx_u32 log = huf_lengths(count, max, len);
        cx_u64 bits_new = 0, bits_old = 0;
        int old_ok = prev->huf_ok;
        for (cx_u32 s = 0; s <= max; s++) {
            if (!count[s]) continue;
            bits_new += (cx_u64)count[s] * len[s];
            const cx_u32 ol = prev->huf[s] >> 16;
            if (!ol) old_ok = 0;
            bits_old += (cx_u64)count[s] * ol;
        }
        cx_u8 tree[160];
        const cx_usize tree_n = huf_tree(len, max, log, tree);
        const int four = n >= 256;
        const cx_usize streams = four ? 6 : 0;
        const cx_usize est_new = tree_n ? tree_n + streams + (bits_new + 7) / 8 + (four ? 3 : 0) : ~(cx_usize)0;
        const cx_usize est_old = old_ok ? streams + (bits_old + 7) / 8 + (four ? 3 : 0) : ~(cx_usize)0;
        const int use_old = old_ok && est_old <= est_new;
        const cx_usize est = use_old ? est_old : est_new;
        if ((use_old || tree_n) && est + 5 < n + raw_head) {
            cx_u32 ct_new[256];
            const cx_u32* ct = prev->huf;
            if (!use_old) {
                huf_codes(len, max, log, ct_new);
                ct = ct_new;
            }
            /* Room for a 5-byte header; moved down if it's shorter. */
            if (cap < 5 + 16) return 0;
            cx_u8* body = out + 5;
            cx_usize at = 0;
            if (!use_old) {
                memcpy(body, tree, tree_n);
                at = tree_n;
            }
            const cx_usize got = huf_streams(ct, lit, n, four, body + at, cap - 5 - at, tmp);
            if (got) {
                const cx_usize csize = at + got;
                cx_usize head;
                cx_u64 h;
                const cx_u32 type = use_old ? 3 : 2;
                if (!four) {
                    head = 3;
                    h = type | ((cx_u64)n << 4) | ((cx_u64)csize << 14);
                } else if (n < 1024 && csize < 1024) {
                    head = 3;
                    h = type | (1u << 2) | ((cx_u64)n << 4) | ((cx_u64)csize << 14);
                } else if (n < 16384 && csize < 16384) {
                    head = 4;
                    h = type | (2u << 2) | ((cx_u64)n << 4) | ((cx_u64)csize << 18);
                } else {
                    head = 5;
                    h = type | (3u << 2) | ((cx_u64)n << 4) | ((cx_u64)csize << 22);
                }
                if (head + csize < n + raw_head && (four || csize < 1024)) {
                    if (head < 5) memmove(out + head, body, csize);
                    for (cx_usize k = 0; k < head; k++) out[k] = (cx_u8)(h >> (8 * k));
                    if (!use_old) {
                        memcpy(next->huf, ct_new, sizeof next->huf);
                        next->huf_log = log;
                        next->huf_ok = 1;
                    }
                    return head + csize;
                }
            }
        }
    }
    if (cap < raw_head + n) return 0;
    const cx_u32 h = n < 32 ? (cx_u32)n << 3 : n < 4096 ? (1u << 2) | ((cx_u32)n << 4) : (3u << 2) | ((cx_u32)n << 4);
    if (raw_head == 1) out[0] = (cx_u8)h;
    else if (raw_head == 2) st16(out, h);
    else st24(out, h);
    memcpy(out + raw_head, lit, n);
    return raw_head + n;
}

/* The predefined tables, built once. */
static tab_t PRE_LL, PRE_OF, PRE_ML;

/* Picks a table for one of the sequences' codes, count[0..max] of n: the predefined one, one symbol
 * repeated, the last block's, or one of its own at the accuracy that comes out smallest. Puts it in
 * *t, writes its description at out, and gives the mode; *took is the bytes. */
static cx_u32 pick_table(tab_t* t, const tab_t* prev, const tab_t* pre, const cx_u32* count, cx_u32 max, cx_u32 n, cx_u32 log_max, cx_u8* out, cx_usize* took) {
    cx_u32 distinct = 0, one = 0;
    for (cx_u32 s = 0; s <= max; s++) {
        distinct += count[s] != 0;
        one = count[s] ? s : one;
    }
    *took = 0;
    if (distinct == 1 && prev->kind == T_RLE && prev->rle == one) {
        *t = *prev;
        return 3;
    }
    /* In 1/256 bits. */
    cx_u64 best = ~(cx_u64)0;
    cx_u32 mode = 0;
    const cx_u64 pre_cost = tab_cost(pre, count, max);
    if (pre_cost != ~(cx_u64)0) {
        best = pre_cost;
        mode = 0;
    }
    if (prev->kind == T_FSE || prev->kind == T_PRE) {
        const cx_u64 c = tab_cost(prev, count, max);
        if (c < best) {
            best = c;
            mode = 3;
        }
    }
    if (distinct == 1) {
        /* One symbol: a byte, and no bits for any of them. */
        if (8 * 256 < best) {
            t->kind = T_RLE;
            t->rle = one;
            t->log = 0;
            t->max = one;
            build_rle(&t->ct, one);
            out[0] = (cx_u8)one;
            *took = 1;
            return 1;
        }
        goto chosen;
    }
    cx_u32 lo = 5;
    while ((1u << lo) < distinct) lo++;
    cx_u32 hi = high_bit(n) + 2;
    if (hi > log_max) hi = log_max;
    if (hi < lo) hi = lo;
    /* The two largest: a smaller table seldom pays for what its description saves. */
    if (hi - lo > 1) lo = hi - 1;
    /* No table of its own codes the counts in fewer bits than their entropy, and its description
     * takes at least `lo` bits for the first count and, for each symbol after, as many as the slots
     * left have below their highest. When that's no less than the best, as on most small inputs
     * after a dictionary, it isn't made. lg8 is from 1.874/256 bits below log2 to 0.494 above, so
     * the entropy computed with it is at most 2.862/256 bits a code over what a table's cost
     * computed with it can come to: 3 a code are taken off. */
    /* The k-th symbol from the last has at least k + 1 slots left when it's written. */
    cx_u32 after = 0;
    for (cx_u32 k = 2; k <= distinct; k++) after += high_bit(k);
    if (best != ~(cx_u64)0) {
        cx_i64 low = (cx_i64)n * lg8(n) - 3 * (cx_i64)n;
        for (cx_u32 s = 0; s <= max; s++) low -= count[s] < CLG_N ? CLG[count[s]] : (cx_i64)count[s] * lg8(count[s]);
        low += (cx_i64)((4 + lo + after + 7) >> 3) << 11;
        if (low >= (cx_i64)best) goto chosen;
    }
    cx_i16 bnorm[64];
    cx_u32 blog = 0;
    cx_u8 bdesc[128];
    cx_usize bdesc_n = 0;
    int fresh = 0;
    for (cx_u32 log = lo; log <= hi && log <= log_max; log++) {
        cx_i16 norm[64];
        const cx_u64 fc = normalize(count, max, n, log, norm);
        /* Not described if even the shortest description wouldn't make it the best. */
        if (fc + ((cx_u64)((4 + log + after + 7) >> 3) << 11) >= best) continue;
        cx_u8 desc[128];
        const cx_usize dn = write_counts(norm, max, log, desc);
        const cx_u64 c = fc + ((cx_u64)dn << 11);
        if (c < best) {
            best = c;
            mode = 2;
            memcpy(bnorm, norm, sizeof norm);
            blog = log;
            memcpy(bdesc, desc, dn);
            bdesc_n = dn;
            fresh = 1;
        }
    }
    if (mode == 2 && fresh) {
        t->kind = T_FSE;
        t->log = blog;
        t->max = max;
        memcpy(t->norm, bnorm, sizeof bnorm);
        build_ct(&t->ct, t->norm, max, blog);
        tab_costs(t);
        memcpy(out, bdesc, bdesc_n);
        *took = bdesc_n;
        return 2;
    }
chosen:
    if (mode == 3) {
        *t = *prev;
        return 3;
    }
    *t = *pre;
    return 0;
}

/* Counts the symbols in s[0..n), each below 64, into four tables at once so that runs of one
 * symbol don't wait on each other, then adds them up into count; gives the highest. */
static cx_u32 histogram(const cx_u8* s, cx_u32 n, cx_u32* count) {
    cx_u32 c[4][64];
    memset(c, 0, sizeof c);
    cx_u32 i = 0;
    for (; i + 4 <= n; i += 4) {
        c[0][s[i]]++;
        c[1][s[i + 1]]++;
        c[2][s[i + 2]]++;
        c[3][s[i + 3]]++;
    }
    for (; i < n; i++) c[0][s[i]]++;
    cx_u32 max = 0;
    for (cx_u32 k = 0; k < 64; k++) {
        count[k] = c[0][k] + c[1][k] + c[2][k] + c[3][k];
        if (count[k]) max = k;
    }
    return max;
}

/* The sequences' section for seq[0..n): their count, their tables' modes and descriptions, and the
 * bitstream. ofb[i] is each one's offset as coded. Their codes go in J->llc and the rest from s0;
 * if known, the splitter has put them there already, and known is their counts. Gives its size, or
 * 0 if it doesn't fit. */
static inline __attribute__((always_inline)) cx_usize seq_section_with(job_t* J, const ent_t* prev, ent_t* next, const seq_t* seq, const cx_u32* ofb, const cx_u32 ofb_step,
                                                                     cx_u32 n, const cx_u32* known, cx_u32 s0, cx_u8* out, cx_usize cap) {
    cx_u8* op = out;
    if (cap < 4 + 3 * 128 + 16) return 0;
    if (n < 128) {
        *op++ = (cx_u8)n;
    } else if (n < 0x7F00) {
        op[0] = (cx_u8)((n >> 8) + 128);
        op[1] = (cx_u8)n;
        op += 2;
    } else {
        op[0] = 255;
        st16(op + 1, n - 0x7F00);
        op += 3;
    }
    if (n == 0) {
        next->ll = prev->ll;
        next->of = prev->of;
        next->ml = prev->ml;
        return (cx_usize)(op - out);
    }
    cx_u8* const llc = J->llc + s0;
    cx_u8* const mlc = J->mlc + s0;
    cx_u8* const ofc = J->ofc + s0;
    cx_u32 lc[64] = {0}, mc[64] = {0}, oc[64] = {0};
    cx_u32 lmax, mmax, omax;
    if (known) {
        memcpy(lc, known + SPLIT_LL, (LL_MAX + 1) * sizeof(cx_u32));
        memcpy(mc, known + SPLIT_ML, (ML_MAX + 1) * sizeof(cx_u32));
        memcpy(oc, known + SPLIT_OF, 32 * sizeof(cx_u32));
        lmax = used_to(lc, LL_MAX + 1) - 1;
        mmax = used_to(mc, ML_MAX + 1) - 1;
        omax = used_to(oc, 32) - 1;
    } else {
        for (cx_u32 i = 0; i < n; i++) {
            llc[i] = (cx_u8)ll_code(seq[i].ll);
            mlc[i] = (cx_u8)ml_code(seq[i].ml - 3);
            ofc[i] = (cx_u8)high_bit(ofb[(cx_usize)i * ofb_step]);
        }
        lmax = histogram(llc, n, lc);
        mmax = histogram(mlc, n, mc);
        omax = histogram(ofc, n, oc);
    }
    cx_u8* const modes = op++;
    cx_usize took;
#ifdef ZSE_CYCLES
    const cx_u64 c0 = __rdtsc();
#endif
    const cx_u32 lm = pick_table(&next->ll, &prev->ll, &PRE_LL, lc, lmax, n, LL_LOG, op, &took);
    op += took;
    const cx_u32 om = pick_table(&next->of, &prev->of, &PRE_OF, oc, omax, n, OF_LOG, op, &took);
    op += took;
    const cx_u32 mm = pick_table(&next->ml, &prev->ml, &PRE_ML, mc, mmax, n, ML_LOG, op, &took);
    op += took;
#ifdef ZSE_CYCLES
    zse_cycles[4] += __rdtsc() - c0;
#endif
    *modes = (cx_u8)((lm << 6) | (om << 4) | (mm << 2));
    const ct_t* const L = &next->ll.ct;
    const ct_t* const O = &next->of.ct;
    const ct_t* const M = &next->ml.ct;
    /* The bits, held in locals: at most 7 are left after each flush, and a sequence adds 26 for
     * its states and 16 for its literals' length before the first and 47 before the second. */
    cx_u8* p = op;
    cx_u8* const pend = out + cap - 16;
    cx_u64 acc = 0;
    cx_u32 nb = 0;
#define ADD(v, k)                         \
    do {                                  \
        acc |= (cx_u64)(v) << nb;         \
        nb += (k);                        \
    } while (0)
#define FLUSH()                           \
    do {                                  \
        st64(p, acc);                     \
        p += nb >> 3;                     \
        acc >>= nb & ~7u;                 \
        nb &= 7;                          \
    } while (0)
#define FSE(ct, x, s)                                              \
    do {                                                           \
        const cx_u32 k_ = ((x) + (ct)->dnb[s]) >> 16;              \
        ADD((x) & ((1u << k_) - 1), k_);                           \
        (x) = (ct)->st[(cx_i32)((x) >> k_) + (ct)->dfs[s]];        \
    } while (0)
    cx_u32 i = n - 1;
    cx_u32 xl = L->first[llc[i]], xo = O->first[ofc[i]], xm = M->first[mlc[i]];
    {
        const cx_u32 l = llc[i], m = mlc[i], o = ofc[i];
        ADD(seq[i].ll - LL_BASE[l], LL_EXTRA[l]);
        FLUSH();
        ADD(seq[i].ml - ML_BASE[m], ML_EXTRA[m]);
        ADD(ofb[(cx_usize)i * ofb_step] - (1u << o), o);
        FLUSH();
    }
    while (i-- > 0) {
        /* The three states' bits and the literals' length go in as one value, put together off
         * the accumulator's path, as do the match's length and offset. Most sequences' bits come
         * to 56 or fewer, which fit beside the 7 a flush leaves, so both go in with one flush. */
        const cx_u32 l = llc[i], m = mlc[i], o = ofc[i];
        const cx_u32 ko = (xo + O->dnb[o]) >> 16, km = (xm + M->dnb[m]) >> 16, kl = (xl + L->dnb[l]) >> 16;
        const cx_u32 kv = ko + km + kl + LL_EXTRA[l], kw = ML_EXTRA[m] + o;
        const cx_u64 v = (cx_u64)(xo & ((1u << ko) - 1)) | (cx_u64)(xm & ((1u << km) - 1)) << ko | (cx_u64)(xl & ((1u << kl) - 1)) << (ko + km) |
                         (cx_u64)(seq[i].ll - LL_BASE[l]) << (ko + km + kl);
        const cx_u64 w = (cx_u64)(seq[i].ml - ML_BASE[m]) | (cx_u64)(ofb[(cx_usize)i * ofb_step] - (1u << o)) << ML_EXTRA[m];
        xo = O->st[(cx_i32)(xo >> ko) + O->dfs[o]];
        xm = M->st[(cx_i32)(xm >> km) + M->dfs[m]];
        xl = L->st[(cx_i32)(xl >> kl) + L->dfs[l]];
        if (__builtin_expect(kv + kw <= 56, 1)) {
            ADD(v | w << kv, kv + kw);
            FLUSH();
        } else {
            ADD(v, kv);
            FLUSH();
            ADD(w, kw);
            FLUSH();
        }
        if (__builtin_expect(p > pend, 0)) return 0;
    }
    ADD(xm - (1u << next->ml.log), next->ml.log);
    ADD(xo - (1u << next->of.log), next->of.log);
    ADD(xl - (1u << next->ll.log), next->ll.log);
    ADD(1, 1);
    FLUSH();
#undef FSE
#undef FLUSH
#undef ADD
    if (p > pend) return 0;
    return (cx_usize)(p - out) + (nb > 0);
}

/* Built for the offsets in the sequences, as they nearly always are, and for ones coded again apart:
 * in the first the offset is read off the same pointer as the lengths. Never inlined: in block(),
 * with everything else, the loop had its states and bits on the stack, stored and loaded again for
 * each sequence. */
static __attribute__((noinline)) cx_usize seq_section(job_t* J, const ent_t* prev, ent_t* next, const seq_t* seq, const cx_u32* ofb, cx_u32 ofb_step, cx_u32 n,
                                                      const cx_u32* known, cx_u32 s0, cx_u8* out, cx_usize cap) {
    if (ofb_step != 1) return seq_section_with(J, prev, next, seq, &seq[0].ofb, sizeof(seq_t) / sizeof(cx_u32), n, known, s0, out, cap);
    return seq_section_with(J, prev, next, seq, ofb, 1, n, known, s0, out, cap);
}

/* Whether src[0..n) is one byte repeated. */
static int all_same(const cx_u8* src, cx_u32 n) {
    for (cx_u32 k = 1; k < n; k++) {
        if (src[k] != src[0]) return 0;
    }
    return 1;
}

/* Writes one block: the sequences seq[0..ns) and literals lit[0..nl), which make src[0..n), coded
 * into out with its header. The match finder coded the offsets with the repeated offsets fin
 * before them, and left fout after. A block that doesn't come out smaller is stored, and one byte
 * repeated is run length coded; either way the tables and the repeated offsets stay as they were,
 * as the decoder leaves them. If known, the splitter has the counts of these literals and codes,
 * and has put the codes in J->llc and the rest from s0. Gives the bytes written. */
static cx_usize emit(job_t* J, const seq_t* seq, cx_u32 ns, const cx_u32* fin, const cx_u32* fout, const cx_u8* lit, cx_usize nl, const cx_u8* src, cx_u32 n, int last,
                     const cx_u32* known, cx_u32 s0, cx_u8* out, cx_usize cap) {
    const ent_t* prev = &J->ent[J->cur];
    ent_t* next = &J->ent[J->cur ^ 1];
    cx_u32 r[3] = {prev->rep[0], prev->rep[1], prev->rep[2]};
    const cx_u32* ofb = &seq[0].ofb;
    cx_u32 step = sizeof(seq_t) / sizeof(cx_u32);
    if (fin[0] == r[0] && fin[1] == r[1] && fin[2] == r[2]) {
        r[0] = fout[0];
        r[1] = fout[1];
        r[2] = fout[2];
    } else {
        /* The decoder's repeated offsets aren't the finder's: each offset is worked out as the
         * finder meant it and coded again. */
        cx_u32 f[3] = {fin[0], fin[1], fin[2]};
        for (cx_u32 i = 0; i < ns; i++) {
            const cx_u32 off = rep_decode(f, seq[i].ll, seq[i].ofb);
            J->ofv[i] = rep_code(r, seq[i].ll, off);
        }
        ofb = J->ofv;
        step = 1;
        known = 0; /* the offsets' codes have changed */
    }
    cx_usize size = 0;
    {
        /* The literals for negative levels go raw, as libzstd sends them, which is what makes those
         * levels fast to decompress too. */
        const int huf = J->p.strat != S_FAST || J->p.tlen == 0;
        /* Into the output when a block's room is there, as it nearly always is, so it isn't
         * copied there after. */
        cx_u8* const tmp = cap >= 3 + BLOCK_ROOM ? out + 3 : J->blk;
#ifdef ZSE_CYCLES
        const cx_u64 c0 = __rdtsc();
#endif
        const cx_usize ls = lit_section(prev, next, lit, nl, huf, known, tmp, BLOCK_ROOM, J->htmp);
#ifdef ZSE_CYCLES
        const cx_u64 c1 = __rdtsc();
        zse_cycles[2] += c1 - c0;
#endif
        if (ls) {
            const cx_usize ss = seq_section(J, prev, next, seq, ofb, step, ns, known, s0, tmp + ls, BLOCK_ROOM - ls);
            if (ss) size = ls + ss;
            DBG("block %u: %u seqs, %zu lits -> lit %zu + seq %zu = %zu\n", n, ns, nl, ls, ss, size);
        }
#ifdef ZSE_CYCLES
        zse_cycles[3] += __rdtsc() - c1;
        zse_cycles[5] += ns;
#endif
        if (size >= n || cap < 3 + size) size = 0;
        if (size && tmp != out + 3) memcpy(out + 3, tmp, size);
    }
    if (size && !(size < 16 && n > 1 && all_same(src, n))) {
        st24(out, (cx_u32)last | (2u << 1) | ((cx_u32)size << 3));
        next->rep[0] = r[0];
        next->rep[1] = r[1];
        next->rep[2] = r[2];
        J->cur ^= 1;
        return 3 + size;
    }
    if (n > 1 && all_same(src, n)) {
        st24(out, (cx_u32)last | (1u << 1) | ((cx_u32)n << 3));
        out[3] = src[0];
        return 4;
    }
    st24(out, (cx_u32)last | ((cx_u32)n << 3));
    memcpy(out + 3, src, n);
    return 3 + n;
}

/* --- splitting blocks ------------------------------------------------------------------------ */

/* A block whose text changes partway, say from code to a stretch of packed binary, comes out
 * smaller as two blocks, each with tables fit to its own part. The block's sequences are cut into
 * pieces of at least the level's piece size, the counts of each piece's literals and codes summed
 * from the block's start, and a run of pieces split in two where the estimate saves the most, then
 * each side again, for as long as a split saves. The estimate is each part's entropy under its own
 * counts, with what its tables and headers cost, in 1/256 bits; the extra bits of the lengths and
 * offsets are the same however the block is cut, so they're left out. */
#define SPLIT_MAX 64
#define SPLIT_BYTES ((SPLIT_MAX + 1) * SPLIT_W * sizeof(cx_u32))

typedef struct {
    cx_u32 seq, lit, pos; /* where a piece starts: its first sequence, literal and byte */
} cut_t;

typedef struct {
    const cx_u32* P; /* row k: the counts before piece k */
    const cut_t* cut;
    cx_u32 lo, hi;        /* the literals' symbols the block uses */
    cx_u32 llk, mlk, ofk; /* and the codes' */
    cx_u64 margin;        /* what a split has to save */
} split_t;

/* What a symbol costs under the predefined tables, in 1/256 bits, or 1 << 16 if they lack it. */
static cx_u32 PRE_COST[3][64];

/* lg8's fractions, 32 bits each, for gathering. */
static cx_u32 FRAC32[256];

/* sum of c lg c over the counts in [b - a), and how many are used. With AVX2, eight at a time: a
 * count's float has its log's whole part in the exponent and the fraction's first eight bits at
 * the top of the mantissa, exactly as lg8 finds them for counts below 2^24. */
static inline cx_u64 clogc(const cx_u32* a, const cx_u32* b, cx_u32 k, cx_u32* used) {
    cx_u64 s = 0;
    cx_u32 u = 0;
    cx_u32 i = 0;
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    for (; i + 8 <= k; i += 8) {
        const __m256i c = _mm256_sub_epi32(_mm256_loadu_si256((const __m256i*)(b + i)), _mm256_loadu_si256((const __m256i*)(a + i)));
        const __m256i f = _mm256_castps_si256(_mm256_cvtepi32_ps(c));
        const __m256i e = _mm256_sub_epi32(_mm256_srli_epi32(f, 23), _mm256_set1_epi32(127));
        const __m256i fr = _mm256_i32gather_epi32((const int*)FRAC32, _mm256_and_si256(_mm256_srli_epi32(f, 15), _mm256_set1_epi32(255)), 4);
        const __m256i lg = _mm256_add_epi32(_mm256_slli_epi32(e, 8), fr);
        const __m256i nz = _mm256_cmpgt_epi32(c, _mm256_setzero_si256());
        const __m256i t = _mm256_and_si256(_mm256_mullo_epi32(c, lg), nz);
        acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(t)));
        acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_extracti128_si256(t, 1)));
        u += (cx_u32)__builtin_popcount((cx_u32)_mm256_movemask_ps(_mm256_castsi256_ps(nz)));
    }
    {
        cx_u64 v[4];
        _mm256_storeu_si256((__m256i*)v, acc);
        s = v[0] + v[1] + v[2] + v[3];
    }
#endif
    for (; i < k; i++) {
        const cx_u32 c = b[i] - a[i];
        if (c) {
            s += c < CLG_N ? CLG[c] : (cx_u64)c * lg8(c);
            u++;
        }
    }
    *used = u;
    return s;
}

/* One code's cost for n sequences: its own table, run length, or the predefined one. */
static cx_u64 code_cost(const cx_u32* a, const cx_u32* b, cx_u32 k, cx_u32 n, const cx_u32* pre) {
    cx_u32 used;
    const cx_u64 s = clogc(a, b, k, &used);
    if (used <= 1) return 8 * 256;
    cx_u64 best = (cx_u64)n * lg8(n) - s + 256 * (16 + 5 * (cx_u64)used);
    cx_u64 p = 0;
    for (cx_u32 i = 0; i < k; i++) p += (cx_u64)(b[i] - a[i]) * pre[i];
    return p < best ? p : best;
}

/* The estimate for pieces [a, b). */
static cx_u64 split_cost(const split_t* S, cx_u32 a, cx_u32 b) {
    const cx_u32* pa = S->P + (cx_usize)a * SPLIT_W;
    const cx_u32* pb = S->P + (cx_usize)b * SPLIT_W;
    const cx_u32 nl = S->cut[b].lit - S->cut[a].lit, ns = S->cut[b].seq - S->cut[a].seq;
    cx_u64 c = 256 * 64;
    if (nl) {
        cx_u32 used;
        const cx_u64 s = clogc(pa + S->lo, pb + S->lo, S->hi - S->lo, &used);
        const cx_u64 raw = (cx_u64)nl * 8 * 256;
        const cx_u64 huf = used <= 1 ? 8 * 256 : (cx_u64)nl * lg8(nl) - s + 256 * (88 + 3 * (cx_u64)used);
        c += huf < raw ? huf : raw;
    }
    if (ns) {
        c += code_cost(pa + SPLIT_LL, pb + SPLIT_LL, S->llk, ns, PRE_COST[0]);
        c += code_cost(pa + SPLIT_ML, pb + SPLIT_ML, S->mlk, ns, PRE_COST[1]);
        c += code_cost(pa + SPLIT_OF, pb + SPLIT_OF, S->ofk, ns, PRE_COST[2]);
    }
    return c;
}

/* Where to cut pieces [a, b), whose estimate is whole, into cuts[nc..): gives the new count. */
static cx_u32 split_find(const split_t* S, cx_u32 a, cx_u32 b, cx_u64 whole, cx_u32* cuts, cx_u32 nc) {
    if (b - a < 2 || whole <= S->margin) return nc;
    cx_u64 best = whole - S->margin, bl = 0, br = 0;
    cx_u32 bk = 0;
    for (cx_u32 k = a + 1; k < b; k++) {
        const cx_u64 l = split_cost(S, a, k), r = split_cost(S, k, b);
        if (l + r < best) {
            best = l + r;
            bk = k;
            bl = l;
            br = r;
        }
    }
    if (!bk) return nc;
    nc = split_find(S, a, bk, bl, cuts, nc);
    cuts[nc++] = bk;
    return split_find(S, bk, b, br, cuts, nc);
}

/* The smallest piece a level cuts a block into, or 0 if it doesn't split. */
static cx_u32 split_piece(const cp_t* p) {
#ifdef ZSE_TUNE
    const char* e = getenv("ZSE_G");
    if (e) return (cx_u32)atoi(e);
#endif
    if (p->strat == S_FAST) return p->tlen ? 0 : 32 << 10;
    if (p->strat == S_DFAST) return 16 << 10;
    return p->strat >= S_BTOPT ? 2 << 10 : 8 << 10;
}

/* What a split has to save, in 1/256 bits: each part's tables take time to build, which the fast
 * levels spend only for a clear gain, such as a stretch that doesn't compress. */
static cx_u64 split_margin(const cp_t* p) {
#ifdef ZSE_TUNE
    const char* e = getenv("ZSE_SM");
    if (e) return (cx_u64)atoi(e) * 8 * 256;
#endif
    const cx_u64 byte = 8 * 256;
    return p->strat <= S_DFAST ? 1024 * byte : p->strat <= S_LAZY2 ? 64 * byte : 0;
}

/* Writes the block the match finder left in J->ss, which makes base[at..at+n), as one block or
 * several: gives the bytes written. fin and fout are the finder's repeated offsets before and
 * after. Each part is at least 1 KiB, so the headers fit in cx_zse_bound. */
static cx_usize split_emit(job_t* J, const cx_u32* fin, const cx_u32* fout, cx_u32 at, cx_u32 n, int last, cx_u8* out, cx_usize cap) {
    const seq_t* const seq = J->ss.seq;
    const cx_u32 ns = J->ss.n;
    const cx_u8* const lit = J->ss.lit;
    const cx_u32 nl = (cx_u32)(J->ss.lp - J->ss.lit);
    const cx_u8* const src = J->base + at;
    const cx_u32 g = split_piece(&J->p);
    cut_t cut[SPLIT_MAX + 1];
    cx_u32 np = 0;
    cut[0].seq = cut[0].lit = cut[0].pos = 0;
    if (g && n >= 2 * g) {
        cx_u32 pos = 0, lo = 0;
        for (cx_u32 i = 0; i < ns; i++) {
            if (pos >= cut[np].pos + g && n - pos >= g && np + 1 < SPLIT_MAX) {
                np++;
                cut[np].seq = i;
                cut[np].lit = lo;
                cut[np].pos = pos;
            }
            pos += seq[i].ll + seq[i].ml;
            lo += seq[i].ll;
        }
    }
    np++;
    cut[np].seq = ns;
    cut[np].lit = nl;
    cut[np].pos = n;
#ifdef ZSE_CYCLES
    const cx_u64 c0 = __rdtsc();
#endif
    /* The counts, which the sections then use too: kept running from the block's start, the
     * literals' in four tables so a byte repeated doesn't wait on its own count, and read off at
     * each piece's end. */
    cx_u32* const P = J->split;
    cx_u32 c[4][256], cc[SPLIT_W - SPLIT_LL];
    memset(c, 0, sizeof c);
    memset(cc, 0, sizeof cc);
    memset(P, 0, SPLIT_W * sizeof(cx_u32));
    for (cx_u32 k = 0; k < np; k++) {
        cx_u32* const row = P + (cx_usize)(k + 1) * SPLIT_W;
        const cx_u32 to = cut[k + 1].lit;
        cx_u32 i = cut[k].lit;
        for (; i + 4 <= to; i += 4) {
            c[0][lit[i]]++;
            c[1][lit[i + 1]]++;
            c[2][lit[i + 2]]++;
            c[3][lit[i + 3]]++;
        }
        for (; i < to; i++) c[0][lit[i]]++;
        for (i = cut[k].seq; i < cut[k + 1].seq; i++) {
            const cx_u32 l = ll_code(seq[i].ll), m = ml_code(seq[i].ml - 3), o = high_bit(seq[i].ofb);
            J->llc[i] = (cx_u8)l;
            J->mlc[i] = (cx_u8)m;
            J->ofc[i] = (cx_u8)o;
            cc[l]++;
            cc[SPLIT_ML - SPLIT_LL + m]++;
            cc[SPLIT_OF - SPLIT_LL + o]++;
        }
        for (int s = 0; s < 256; s++) row[s] = c[0][s] + c[1][s] + c[2][s] + c[3][s];
        memcpy(row + SPLIT_LL, cc, sizeof cc);
    }
    cx_u32 cuts[SPLIT_MAX + 1];
    cx_u32 nc = 0;
    if (np > 1) {
        const cx_u32* const all = P + (cx_usize)np * SPLIT_W;
        split_t S = {P, cut, 0, used_to(all, 256), used_to(all + SPLIT_LL, LL_MAX + 1), used_to(all + SPLIT_ML, ML_MAX + 1), used_to(all + SPLIT_OF, 32), split_margin(&J->p)};
        while (S.lo < S.hi && !all[S.lo]) S.lo++;
        nc = split_find(&S, 0, np, split_cost(&S, 0, np), cuts, 0);
    }
    cuts[nc] = np;
#ifdef ZSE_CYCLES
    zse_cycles[6] += __rdtsc() - c0;
#endif
    cx_u32 f[3] = {fin[0], fin[1], fin[2]};
    cx_u32 d[SPLIT_W];
    cx_usize got = 0;
    cx_u32 a = 0;
    for (cx_u32 j = 0; j <= nc; j++) {
        const cx_u32 b = cuts[j];
        const cut_t *ca = &cut[a], *cb = &cut[b];
        const cx_u32 *pa = P + (cx_usize)a * SPLIT_W, *pb = P + (cx_usize)b * SPLIT_W;
        for (cx_u32 k = 0; k < SPLIT_W; k++) d[k] = pb[k] - pa[k];
        cx_u32 fo[3] = {fout[0], fout[1], fout[2]};
        if (j < nc) {
            fo[0] = f[0];
            fo[1] = f[1];
            fo[2] = f[2];
            for (cx_u32 i = ca->seq; i < cb->seq; i++) rep_decode(fo, seq[i].ll, seq[i].ofb);
        }
        got += emit(J, seq + ca->seq, cb->seq - ca->seq, f, fo, lit + ca->lit, cb->lit - ca->lit, src + ca->pos, cb->pos - ca->pos, last && j == nc, d, ca->seq,
                    out + got, cap - got);
        f[0] = fo[0];
        f[1] = fo[1];
        f[2] = fo[2];
        a = b;
    }
    return got;
}

static void job_clear(job_t* J);
static void job_prefix(job_t* J);

/* Finds the matches in base[at..at+n), at most a block, and writes them as a block: gives the
 * bytes written. The match finder starts from the repeated offsets the decoder will have. */
static cx_usize block(job_t* J, cx_u32 at, cx_u32 n, int last, cx_u8* out, cx_usize cap) {
    const ent_t* prev = &J->ent[J->cur];
    const cx_u32 fin[3] = {prev->rep[0], prev->rep[1], prev->rep[2]};
    cx_u32 r[3] = {fin[0], fin[1], fin[2]};
    J->ss.n = 0;
    J->ss.lp = J->ss.lit;
#ifdef ZSE_CYCLES
    const cx_u64 t0 = __rdtsc();
#endif
    switch (J->p.strat) {
    case S_FAST: mf_fast(J, at, at + n, r); break;
    case S_DFAST: mf_dfast(J, at, at + n, r); break;
    case S_GREEDY: mf_greedy(J, at, at + n, r); break;
    case S_LAZY: mf_lazy1(J, at, at + n, r); break;
    case S_LAZY2: mf_lazy2(J, at, at + n, r); break;
    case S_BTLAZY2: mf_btlazy(J, at, at + n, r); break;
    default:
        if (J->p.strat == S_BTULTRA2 && !J->st->ready && !J->dict) {
            /* The job's first block is parsed once just for the counts, which then price the parse
             * that's kept; the trees forget the first parse, as their positions would be ahead of
             * the second's. Not after a dictionary, as libzstd doesn't: the trees would take its
             * content in again, which on a small input is most of the work. */
            cx_u32 r1[3] = {fin[0], fin[1], fin[2]};
            mf_opt(J, at, at + n, r1);
            J->ss.n = 0;
            J->ss.lp = J->ss.lit;
            job_clear(J);
            job_prefix(J);
        }
        mf_opt(J, at, at + n, r);
        break;
    }
#ifdef ZSE_CYCLES
    const cx_u64 t1 = __rdtsc();
    zse_cycles[0] += t1 - t0;
#endif
    const cx_usize got = split_emit(J, fin, r, at, n, last, out, cap);
#ifdef ZSE_CYCLES
    zse_cycles[1] += __rdtsc() - t1;
#endif
    return got;
}

/* --- jobs ------------------------------------------------------------------------------------ */

static int tables_ready;

static void tables_init(void) {
    for (cx_u32 i = 0; i < 256; i++) {
        /* log2((256 + i) / 256) to 8 bits, by squaring in 16-bit fixed point. */
        cx_u64 m = (cx_u64)(256 + i) << 7;
        cx_u32 f = 0;
        for (int b = 0; b < 9; b++) {
            m = (m * m) >> 15;
            f <<= 1;
            if (m >= (1u << 16)) {
                f |= 1;
                m >>= 1;
            }
        }
        FRAC[i] = (cx_u16)((f + 1) >> 1);
    }
    for (cx_u32 c = 0; c <= LL_MAX; c++) {
        for (cx_u32 v = LL_BASE[c]; v < 64 && v < LL_BASE[c] + (1u << LL_EXTRA[c]); v++) LL_CODE[v] = (cx_u8)c;
    }
    for (cx_u32 c = 0; c <= ML_MAX; c++) {
        for (cx_u32 v = ML_BASE[c] - 3; v < 128 && v < ML_BASE[c] - 3 + (1u << ML_EXTRA[c]); v++) ML_CODE[v] = (cx_u8)c;
    }
    PRE_LL.kind = PRE_OF.kind = PRE_ML.kind = T_PRE;
    PRE_LL.log = 6;
    PRE_LL.max = 35;
    memcpy(PRE_LL.norm, LL_NORM, sizeof LL_NORM);
    build_ct(&PRE_LL.ct, PRE_LL.norm, 35, 6);
    PRE_ML.log = 6;
    PRE_ML.max = 52;
    memcpy(PRE_ML.norm, ML_NORM, sizeof ML_NORM);
    build_ct(&PRE_ML.ct, PRE_ML.norm, 52, 6);
    PRE_OF.log = 5;
    PRE_OF.max = 28;
    memcpy(PRE_OF.norm, OF_NORM, sizeof OF_NORM);
    build_ct(&PRE_OF.ct, PRE_OF.norm, 28, 5);
    tab_costs(&PRE_LL);
    tab_costs(&PRE_ML);
    tab_costs(&PRE_OF);
    for (cx_u32 c = 1; c < CLG_N; c++) CLG[c] = c * lg8(c);
    for (int i = 0; i < 256; i++) FRAC32[i] = FRAC[i];
    const tab_t* pre[3] = {&PRE_LL, &PRE_ML, &PRE_OF};
    for (int t = 0; t < 3; t++) {
        for (cx_u32 s = 0; s < 64; s++) {
            const cx_i32 c = s <= pre[t]->max ? pre[t]->norm[s] : 0;
            PRE_COST[t][s] = c == 0 ? 1u << 16 : (pre[t]->log << 8) - lg8(c < 0 ? 1 : (cx_u32)c);
        }
    }
    __atomic_store_n(&tables_ready, 1, __ATOMIC_RELEASE);
}

/* The settings for a level and an input of n bytes: the window and the tables no bigger than the
 * input needs. */
static cp_t params_for(cx_i32 level, cx_u64 n) {
    const cp_t* const t = LEVELS[(n <= 256 * 1024) + (n <= 128 * 1024) + (n <= 16 * 1024)];
    cp_t p;
    if (level < 0) {
        p = t[0];
        p.tlen = (cx_u32)(-(cx_i64)level);
    } else {
        p = t[level == 0 ? 3 : level > 22 ? 22 : level];
    }
#ifdef ZSE_TUNE
    {
        /* ZSE_P=wlog,clog,hlog,slog,mml,tlen,strat, any left empty kept. */
        const char* e = getenv("ZSE_P");
        cx_u32* f[7] = {&p.wlog, &p.clog, &p.hlog, &p.slog, &p.mml, &p.tlen, &p.strat};
        for (int k = 0; e && *e && k < 7; k++) {
            if (*e != ',') *f[k] = (cx_u32)strtoul(e, (char**)&e, 10);
            if (*e == ',') e++;
        }
    }
#endif
    cx_u32 need = 10;
    while (need < 31 && ((cx_u64)1 << need) < n) need++;
    if (p.wlog > need) p.wlog = need;
    const cx_u32 bt = p.strat >= S_BTLAZY2;
    if (p.hlog > p.wlog + 1) p.hlog = p.wlog + 1;
    if (p.clog > p.wlog + bt) p.clog = p.wlog + bt;
    if (p.hlog < 6) p.hlog = 6;
    if (p.clog < 6) p.clog = 6;
    return p;
}

/* How much text a job takes, and how far back before it its matches reach. */
static cx_u64 job_size(const cp_t* p) {
    const cx_u64 w = (cx_u64)1 << p->wlog;
#ifdef ZSE_ONE_JOB
    return (cx_u64)1 << 40;
#endif
    cx_u64 s = 4 * w;
    if (s < ((cx_u64)1 << 20)) s = (cx_u64)1 << 20;
    return s;
}

static cx_u64 job_reach(const cp_t* p) {
    const cx_u64 w = (cx_u64)1 << p->wlog;
    return p->strat >= S_BTOPT ? w : p->strat >= S_LAZY2 ? w / 2 : w / 8;
}

/* Empties the match finder's tables. */
static void job_clear(job_t* J) {
    if (J->hash) memset(J->hash, 0, J->hash_n * 4);
    if (J->chain) memset(J->chain, 0, J->chain_n * 4);
    if (J->hash3) memset(J->hash3, 0, (cx_usize)4 << HASH3_LOG);
    J->next_up = J->start;
}

static void job_tables(job_t* J) {
    cx_usize hn = 0, cn = 0;
    switch (J->p.strat) {
    case S_FAST: hn = (cx_usize)1 << J->p.hlog; break;
    case S_DFAST:
        hn = (cx_usize)1 << J->p.hlog;
        cn = (cx_usize)1 << J->p.clog;
        break;
    case S_GREEDY:
    case S_LAZY:
    case S_LAZY2:
        /* The rows' positions, then in the second table their tags, with room to read a row's
         * past the last. */
        J->rlog = row_log(J->p.slog);
        hn = (cx_usize)1 << J->p.hlog;
        cn = (hn + 64) / 4;
        break;
    default:
        hn = (cx_usize)1 << J->p.hlog;
        cn = (cx_usize)1 << J->p.clog;
        break;
    }
    /* With a working copy of a dictionary's, the job uses that instead. */
    if (J->dw) hn = cn = 0;
    if (J->hash_n != hn) {
        if (J->hash) cx_free(J->hash, J->hash_n * 4);
        J->hash = hn ? (cx_u32*)cx_alloc(hn * 4) : 0;
        J->hash_n = hn;
    }
    if (J->chain_n != cn) {
        if (J->chain) cx_free(J->chain, J->chain_n * 4);
        J->chain = cn ? (cx_u32*)cx_alloc(cn * 4) : 0;
        J->chain_n = cn;
    }
    if (J->p.strat >= S_GREEDY && J->p.strat <= S_LAZY2) J->tags = (cx_u8*)J->chain;
    J->bt_mls = J->p.mml < 4 ? 4 : J->p.mml;
    J->bt_up = 1u << J->p.slog;
#ifdef ZSE_TUNE
    if (getenv("ZSE_UD")) J->bt_up >>= atoi(getenv("ZSE_UD"));
    if (!J->bt_up) J->bt_up = 1;
#endif
    const int three = J->p.strat >= S_BTLAZY2 && J->p.mml == 3;
    if (three && !J->hash3 && !J->dw) J->hash3 = (cx_u32*)cx_alloc((cx_usize)4 << HASH3_LOG);
    if (!three && J->hash3) {
        cx_free(J->hash3, (cx_usize)4 << HASH3_LOG);
        J->hash3 = 0;
    }
    if (J->p.strat >= S_BTLAZY2 && !J->mt) J->mt = (match_t*)cx_alloc(MATCHES_MAX * sizeof(match_t));
    if (J->p.strat >= S_BTOPT && !J->opt) {
        J->st = (stats_t*)cx_alloc(sizeof(stats_t));
        J->opt = (node_t*)cx_alloc((OPT_NUM + 8) * sizeof(node_t));
        J->path = (match_t*)cx_alloc(OPT_NUM * sizeof(match_t));
        J->path_at = (cx_u32*)cx_alloc(OPT_NUM * sizeof(cx_u32));
    }
    if (J->st) J->st->ready = 0;
}

/* Takes the text before the job into its tables, as its matches may reach it. */
static void job_prefix(job_t* J) {
    const cx_u8* const base = J->base;
    const cx_u32 from = J->start, to = J->from;
    if (to - from < READ) return;
    const cx_u32 end = to - READ;
    switch (J->p.strat) {
    case S_FAST:
        for (cx_u32 i = from; i < end; i++) J->hash[hash_at(base + i, J->p.mml, J->p.hlog)] = i;
        break;
    case S_DFAST:
        for (cx_u32 i = from; i < end; i++) {
            J->hash[hash_at(base + i, 8, J->p.hlog)] = i;
            J->chain[hash_at(base + i, J->p.mml, J->p.clog)] = i;
        }
        break;
    case S_GREEDY:
    case S_LAZY:
    case S_LAZY2: {
        rw_t w;
        rw_load(&w, J);
        for (cx_u32 i = from; i < end; i++) row_put(&w, i, row_hash(&w, i));
        J->next_up = end;
        break;
    }
    case S_BTLAZY2:
        J->next_up = from;
        if (!J->dict) {
            dt_update(J, end);
            break;
        }
        /* A dictionary's content goes in sorted, as libzstd does it, rather than for each frame's
         * searches to sort it again: its trees are kept (digest_take). */
        /* fall through */
    default:
        J->next_up = from;
        if (J->dict) {
            J->lim = to;
            J->cut = 1;
        }
        bt_update(J, end, base + to);
        J->lim = J->to;
        J->cut = 0;
        break;
    }
}

/* A trained dictionary's tables and repeated offsets, which the decoder starts the frame with, so
 * the first block may repeat them: from calx_zstd.c's cx_zstd_dict_params, which says how they're
 * laid out. */
#define DP_HUF 256
#define DP_FSE 258
#define DP_REP (258 + 3 * 130)

static void dict_build(ent_t* e, const cx_u8* P) {
    huf_codes(P, P[DP_HUF + 1], P[DP_HUF], e->huf);
    e->huf_log = P[DP_HUF];
    e->huf_ok = 1;
    tab_t* const t[3] = {&e->of, &e->ml, &e->ll};
    for (int k = 0; k < 3; k++) {
        const cx_u8* const q = P + DP_FSE + 130 * k;
        tab_t* const x = t[k];
        x->kind = T_FSE;
        x->log = q[0];
        x->max = q[1];
        x->rle = 0;
        memset(x->norm, 0, sizeof x->norm);
        for (cx_u32 s = 0; s <= x->max; s++) x->norm[s] = (cx_i16)(cx_u16)(q[2 + 2 * s] | (cx_u32)q[3 + 2 * s] << 8);
        build_ct(&x->ct, x->norm, x->max, x->log);
        tab_costs(x);
    }
    for (int i = 0; i < 3; i++) e->rep[i] = ld32(P + DP_REP + 4 * i);
}

/* --- dictionaries' tables, kept -------------------------------------------------------------- */

/* A dictionary's content taken into the match finder's tables, for one level's settings, kept so
 * that a later frame with the same content and settings copies the tables rather than taking the
 * content in again, which on a small input is most of the work. One is found by its settings and
 * by its content compared byte for byte, as the trees take what they were built from on trust. At
 * most DIGEST_MAX are kept, DIGEST_BYTES in all, the least recently used let go first. They're
 * the system's memory, not the runtime allocator's, as they're kept until the program ends. */
#define DIGEST_MAX 16
#define DIGEST_BYTES ((cx_usize)32 << 20)
/* Content shorter than this goes into the tables faster than they'd copy. */
#define DIGEST_MIN 256

/* A copy of a digest's tables that a frame at the hash-table and hash-row levels uses where it is,
 * and puts back as it was after, by the entries its own positions led to: for a small input that's
 * far less than copying all of them. */
typedef struct work_s {
    struct work_s* next;
    cx_usize bytes;
    cx_u32* hash;
    cx_u32* chain;
    cx_u32* hash3;
    cx_u32* log; /* at the tree levels, where the job notes its writes into the copy's nodes */
    cx_u32 log_cap;
    cx_u8* text; /* the content, then room for WORK_TEXT bytes of a frame's text */
} work_t;

/* The writes into a working copy's nodes a frame at the tree levels notes, to undo them: past
 * these, the trees are copied back whole. A position of a small input notes about as many as the
 * levels search deep. */
#define WORK_LOG ((cx_u32)32 << 10)

/* A first job with no more text than this has it after the working copy's content, rather than
 * the content and its text copied into a buffer of their own. */
#define WORK_TEXT ((cx_usize)64 << 10)

typedef struct digest_s {
    struct digest_s* next;
    cp_t p;
    cx_u32 pre;     /* the content's last `pre` bytes, which the tables hold */
    cx_u64 key;     /* the dictionary's key, or 0: then it's found by its content */
    cx_u32 next_up; /* the job's next_up after them */
    cx_u32 refs;    /* frames using it now */
    cx_u64 used;
    cx_usize bytes, hash_n, chain_n, hash3_n; /* bytes: its own and its working copies' */
    cx_u32* hash;
    cx_u32* chain;
    cx_u32* hash3;
    cx_u8* text;
    work_t* work; /* its working copies that no frame has now */
} digest_t;

static digest_t* digests;
static cx_u64 digest_clock;
static cx_usize digest_total, digest_count;
static char digest_busy;

static void digest_lock(void) {
    while (__atomic_test_and_set(&digest_busy, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&digest_busy, __ATOMIC_RELAXED)) {}
}

static void digest_unlock(void) { __atomic_clear(&digest_busy, __ATOMIC_RELEASE); }

/* Whether d is for settings p and the content's last pre bytes at text: by the dictionary's key if
 * it has one, which no other dictionary has and whose content can't change, or else by the
 * content's ends, the whole of it compared after, outside the lock. */
static int digest_like(const digest_t* d, const cp_t* p, cx_u32 pre, const cx_u8* text, cx_u64 key) {
    if (d->pre != pre || memcmp(&d->p, p, sizeof *p)) return 0;
    if (key) return d->key == key;
    const cx_u32 e = pre < 16 ? pre : 16;
    return !memcmp(d->text, text, e) && !memcmp(d->text + pre - e, text + pre - e, e);
}

static work_t* work_new(digest_t* d);
static void work_give(digest_t* d, work_t* w);

static void digest_release(digest_t* d) {
    digest_lock();
    d->refs--;
    digest_unlock();
}

/* The kept digest for settings p and a dictionary's key, or its content's last pre bytes at text,
 * held until digest_release or work_give, or 0. With wp, *wp is a working copy of it for the frame,
 * one no frame has now or a new one, or 0 if there's no memory for one: one lock for both. */
static digest_t* digest_find(const cp_t* p, cx_u32 pre, const cx_u8* text, cx_u64 key, work_t** wp) {
    if (wp) *wp = 0;
    if (pre < DIGEST_MIN) return 0;
#ifdef ZSE_TUNE
    if (getenv("ZSE_NODIGEST")) return 0;
#endif
    digest_t* d;
    work_t* w = 0;
    digest_lock();
    for (d = digests; d; d = d->next)
        if (digest_like(d, p, pre, text, key)) break;
    if (d) {
        d->refs++;
        d->used = ++digest_clock;
        if (wp && (w = d->work)) d->work = w->next;
    }
    digest_unlock();
    if (!d) return 0;
    if (!key && memcmp(d->text, text, pre)) {
        if (w) work_give(d, w);
        else digest_release(d);
        return 0;
    }
    if (wp) *wp = w ? w : work_new(d);
    return d;
}

/* A new working copy of d's tables and content, or 0 if there's no memory for one. */
static work_t* work_new(digest_t* d) {
    const cx_u32 log_cap = d->p.strat >= S_BTLAZY2 ? WORK_LOG : 0;
    const cx_usize bytes = sizeof(work_t) + 4 * (d->hash_n + d->chain_n + d->hash3_n + 2 * (cx_usize)log_cap) + d->pre + WORK_TEXT;
    work_t* const w = (work_t*)malloc(bytes);
    if (!w) return 0;
    w->bytes = bytes;
    w->hash = (cx_u32*)(w + 1);
    w->chain = w->hash + d->hash_n;
    w->hash3 = d->hash3_n ? w->chain + d->chain_n : 0;
    w->log = w->chain + d->chain_n + d->hash3_n;
    w->log_cap = log_cap;
    w->text = (cx_u8*)(w->log + 2 * (cx_usize)log_cap);
    memcpy(w->hash, d->hash, d->hash_n * 4);
    if (d->chain_n) memcpy(w->chain, d->chain, d->chain_n * 4);
    if (d->hash3_n) memcpy(w->hash3, d->hash3, d->hash3_n * 4);
    memcpy(w->text, d->text, d->pre);
    digest_lock();
    d->bytes += bytes;
    digest_total += bytes;
    digest_unlock();
    return w;
}

/* Puts w back as d has it, after J's frame: each entry, or for hash rows each row, that one of the
 * frame's positions leads to, or all of them for an input long enough that copying is less. */
static void work_clean(const job_t* J, const digest_t* d, work_t* w) {
    const cx_u8* const base = J->base;
    const cx_u32 lo = J->from - READ, hi = J->to - J->from >= READ ? J->to - READ + 1 : J->from - READ;
    const cx_usize n = hi - lo;
    const cx_u32 mml = J->p.mml;
    switch (J->p.strat) {
    case S_FAST:
        if (n * 16 > d->hash_n) {
            memcpy(w->hash, d->hash, d->hash_n * 4);
            break;
        }
        for (cx_u32 i = lo; i < hi; i++) {
            const cx_u32 h = hash_at(base + i, mml, J->p.hlog);
            w->hash[h] = d->hash[h];
        }
        break;
    case S_DFAST:
        if (n * 16 > d->hash_n + d->chain_n) {
            memcpy(w->hash, d->hash, d->hash_n * 4);
            memcpy(w->chain, d->chain, d->chain_n * 4);
            break;
        }
        for (cx_u32 i = lo; i < hi; i++) {
            const cx_u32 h = hash_at(base + i, 8, J->p.hlog), c = hash_at(base + i, mml, J->p.clog);
            w->hash[h] = d->hash[h];
            w->chain[c] = d->chain[c];
        }
        break;
    case S_GREEDY:
    case S_LAZY:
    case S_LAZY2: {
        /* Hash rows: each row's positions, and its tags with its head first, copied whole with
         * the row's size known, so the copies are a few vector moves. */
        const cx_u32 rlog = J->rlog, hb = J->p.hlog - rlog + 8;
        cx_u32* const pos = w->hash;
        cx_u8* const tags = (cx_u8*)w->chain;
        const cx_u32* const dpos = d->hash;
        const cx_u8* const dtags = (const cx_u8*)d->chain;
        if ((n << rlog) * 2 > d->hash_n) {
            memcpy(w->hash, d->hash, d->hash_n * 4);
            memcpy(w->chain, d->chain, d->chain_n * 4);
            break;
        }
#define ROWS_BACK(R)                                                      \
    for (cx_u32 i = lo; i < hi; i++) {                                    \
        const cx_u32 at = (hash_at(base + i, mml, hb) >> 8) << (R);       \
        memcpy(pos + at, dpos + at, (cx_usize)4 << (R));                  \
        memcpy(tags + at, dtags + at, (cx_usize)1 << (R));                \
    }
        if (rlog == 4) ROWS_BACK(4)
        else if (rlog == 5) ROWS_BACK(5)
        else ROWS_BACK(6)
#undef ROWS_BACK
        break;
    }
    default: {
        /* Trees. The nodes the copy came with, of the positions before d->next_up, are as they
         * were once the writes into them are undone, newest first. The frame's own nodes, and the
         * hash tables' entries for its positions, are put back from d after, as positions far
         * enough on take the slots of ones out of the window. */
        const cx_u32 mask = (1u << (J->p.clog - 1)) - 1, blo = d->next_up;
        const cx_u32 end = J->to - J->from >= READ ? J->to - READ + 1 : blo;
        const cx_u32 bhi = J->next_up < end ? J->next_up : end;
        const cx_usize bn = bhi > blo ? bhi - blo : 0;
        if (J->ulog_n > J->ulog_cap || bn * 64 > d->chain_n) {
            memcpy(w->hash, d->hash, d->hash_n * 4);
            memcpy(w->chain, d->chain, d->chain_n * 4);
            if (d->hash3_n) memcpy(w->hash3, d->hash3, d->hash3_n * 4);
            break;
        }
        for (cx_u32 k = J->ulog_n; k-- > 0;) w->chain[J->ulog[2 * k]] = J->ulog[2 * k + 1];
        for (cx_u32 a = blo; a < bhi;) {
            const cx_u32 at = a & mask, run = bhi - a < mask + 1 - at ? bhi - a : mask + 1 - at;
            memcpy(w->chain + 2 * (cx_usize)at, d->chain + 2 * (cx_usize)at, (cx_usize)run * 8);
            a += run;
        }
        for (cx_u32 i = blo; i < bhi; i++) {
            const cx_u32 h = hash_at(base + i, J->bt_mls, J->p.hlog);
            w->hash[h] = d->hash[h];
        }
        if (d->hash3_n) {
            for (cx_u32 i = blo; i < bhi; i++) {
                const cx_u32 h = hash_at(base + i, 3, HASH3_LOG);
                w->hash3[h] = d->hash3[h];
            }
        }
        break;
    }
    }
}

/* Gives w back to d, and lets d go. */
static void work_give(digest_t* d, work_t* w) {
    digest_lock();
    w->next = d->work;
    d->work = w;
    d->refs--;
    digest_unlock();
}

static void digest_free(digest_t* d) {
    for (work_t* w = d->work; w;) {
        work_t* const next = w->next;
        free(w);
        w = next;
    }
    free(d);
}

/* Keeps J's tables, with its dictionary's content just taken in, for the frames after. */
static void digest_keep(const job_t* J) {
    const cx_u32 pre = J->from - J->start;
    const cx_u8* const text = J->base + J->start;
    if (pre < DIGEST_MIN) return;
    const cx_usize h3 = J->hash3 ? (cx_usize)1 << HASH3_LOG : 0;
    const cx_usize bytes = sizeof(digest_t) + 4 * (J->hash_n + J->chain_n + h3) + pre;
    if (bytes > DIGEST_BYTES / 4) return;
    digest_t* d = (digest_t*)malloc(bytes);
    if (!d) return;
    d->p = J->p;
    d->pre = pre;
    d->key = J->dkey;
    d->next_up = J->next_up;
    d->refs = 0;
    d->work = 0;
    d->bytes = bytes;
    d->hash_n = J->hash_n;
    d->chain_n = J->chain_n;
    d->hash3_n = h3;
    d->hash = (cx_u32*)(d + 1);
    d->chain = d->hash + d->hash_n;
    d->hash3 = d->chain + d->chain_n;
    d->text = (cx_u8*)(d->hash3 + h3);
    memcpy(d->hash, J->hash, d->hash_n * 4);
    if (d->chain_n) memcpy(d->chain, J->chain, d->chain_n * 4);
    if (h3) memcpy(d->hash3, J->hash3, h3 * 4);
    memcpy(d->text, text, pre);
    digest_lock();
    for (digest_t* e = digests; e; e = e->next) {
        /* Another thread kept it first. */
        if (digest_like(e, &J->p, pre, text, J->dkey) && (J->dkey || !memcmp(e->text, text, pre))) {
            digest_unlock();
            free(d);
            return;
        }
    }
    d->used = ++digest_clock;
    d->next = digests;
    digests = d;
    digest_count++;
    digest_total += bytes;
    while (digest_count > DIGEST_MAX || digest_total > DIGEST_BYTES) {
        digest_t** oldest = 0;
        for (digest_t** at = &digests; *at; at = &(*at)->next)
            if (*at != d && !(*at)->refs && (!oldest || (*at)->used < (*oldest)->used)) oldest = at;
        if (!oldest) break;
        digest_t* const v = *oldest;
        *oldest = v->next;
        digest_count--;
        digest_total -= v->bytes;
        digest_free(v);
    }
    digest_unlock();
}

/* A trained dictionary's tables, built: kept for the last DICT_ENT dictionaries, as building
 * them, three FSE tables and a Huffman code, costs more than all of a small input's matches. */
#define DICT_ENT 4
#define DP_ALL (DP_REP + 12)

typedef struct {
    cx_u8 key[DP_ALL];
    cx_u64 dkey; /* the dictionary's key, or 0 */
    cx_u32 huf[256];
    cx_u32 huf_log;
    tab_t of, ml, ll;
    cx_u32 rep[3];
} dict_ent_t;

static dict_ent_t* dict_ents[DICT_ENT];
static cx_u32 dict_ent_next;

static void dict_put(ent_t* e, const dict_ent_t* k) {
    memcpy(e->huf, k->huf, sizeof e->huf);
    e->huf_log = k->huf_log;
    e->huf_ok = 1;
    e->of = k->of;
    e->ml = k->ml;
    e->ll = k->ll;
    for (int i = 0; i < 3; i++) e->rep[i] = k->rep[i];
}

static void dict_tables(ent_t* e, const cx_u8* P, cx_u64 dkey) {
    digest_lock();
    for (int i = 0; i < DICT_ENT; i++) {
        if (dict_ents[i] && (dkey ? dict_ents[i]->dkey == dkey : !memcmp(dict_ents[i]->key, P, DP_ALL))) {
            dict_put(e, dict_ents[i]);
            digest_unlock();
            return;
        }
    }
    digest_unlock();
    dict_build(e, P);
    dict_ent_t* const k = (dict_ent_t*)malloc(sizeof(dict_ent_t));
    if (!k) return;
    memcpy(k->key, P, DP_ALL);
    k->dkey = dkey;
    memcpy(k->huf, e->huf, sizeof k->huf);
    k->huf_log = e->huf_log;
    k->of = e->of;
    k->ml = e->ml;
    k->ll = e->ll;
    for (int i = 0; i < 3; i++) k->rep[i] = e->rep[i];
    digest_lock();
    const cx_u32 at = dict_ent_next++ % DICT_ENT;
    dict_ent_t* const old = dict_ents[at];
    dict_ents[at] = k;
    digest_unlock();
    free(old);
}

static void job_blocks(job_t* J);

/* Compresses the job's text into its blocks. With a dictionary kept for its settings, the
 * hash-table and hash-row levels use a working copy of its tables where it is, and the trees copy
 * them, as their links change all over and their searches cost far more than the copy. */
static void job_run(job_t* J) {
    job_tables(J);
    digest_t* d = J->dg;
    work_t* const w = J->dw;
    cx_u32* const own_hash = J->hash;
    cx_u32* const own_chain = J->chain;
    cx_u32* const own_hash3 = J->hash3;
    cx_u8* const own_tags = J->tags;
    if (d) {
        J->next_up = d->next_up;
        if (w) {
            J->hash = w->hash;
            J->chain = w->chain;
            if (J->p.strat >= S_GREEDY && J->p.strat <= S_LAZY2) J->tags = (cx_u8*)w->chain;
            if (J->p.strat >= S_BTLAZY2) {
                J->hash3 = w->hash3;
                J->ulog = w->log;
                J->ulog_n = 0;
                J->ulog_cap = w->log_cap;
                J->ulo = d->next_up;
            }
        } else {
            memcpy(J->hash, d->hash, d->hash_n * 4);
            if (d->chain_n) memcpy(J->chain, d->chain, d->chain_n * 4);
            if (d->hash3_n) memcpy(J->hash3, d->hash3, d->hash3_n * 4);
            digest_release(d);
            d = 0;
        }
    } else {
        job_clear(J);
        job_prefix(J);
        if (J->dict) digest_keep(J);
    }
    job_blocks(J);
    if (w) {
        work_clean(J, d, w);
        J->hash = own_hash;
        J->chain = own_chain;
        J->hash3 = own_hash3;
        J->tags = own_tags;
        J->ulo = 0;
        work_give(d, w);
    }
}

static void job_blocks(job_t* J) {
    ent_t* e = &J->ent[0];
    J->cur = 0;
    e->huf_ok = 0;
    e->ll.kind = e->of.kind = e->ml.kind = T_NONE;
    /* The first job starts with the frame's repeated offsets, and a trained dictionary's tables and
     * offsets; the others can't know the ones the job before left, so they have none until their
     * matches make them. */
    if (J->first) {
        e->rep[0] = 1;
        e->rep[1] = 4;
        e->rep[2] = 8;
        if (J->dparams) dict_tables(e, J->dparams, J->dkey);
    } else {
        e->rep[0] = e->rep[1] = e->rep[2] = 0;
    }
    J->ss.src_end = J->base + J->to;
    cx_u32 at = J->from;
    J->out_n = 0;
    if (at == J->to) {
        /* No text: one empty block, the last. */
        st24(J->out, (cx_u32)J->last);
        J->out_n = 3;
        return;
    }
    while (at < J->to) {
        const cx_u32 n = J->to - at < BLOCK_MAX ? J->to - at : BLOCK_MAX;
        const int last = J->last && at + n == J->to;
        J->out_n += block(J, at, n, last, J->out + J->out_n, J->out_cap - J->out_n);
        at += n;
    }
}

static void job_free(job_t* J) {
    if (J->hash) cx_free(J->hash, J->hash_n * 4);
    if (J->chain) cx_free(J->chain, J->chain_n * 4);
    if (J->ss.seq) cx_free(J->ss.seq, SEQ_MAX * sizeof(seq_t));
    if (J->ss.lit) cx_free(J->ss.lit, BLOCK_MAX + 32);
    if (J->blk) cx_free(J->blk, BLOCK_ROOM);
    if (J->split) cx_free(J->split, SPLIT_BYTES);
    if (J->htmp) cx_free(J->htmp, 4 * HUF_ROOM);
    if (J->hash3) cx_free(J->hash3, (cx_usize)4 << HASH3_LOG);
    if (J->mt) cx_free(J->mt, MATCHES_MAX * sizeof(match_t));
    if (J->opt) {
        cx_free(J->st, sizeof(stats_t));
        cx_free(J->opt, (OPT_NUM + 8) * sizeof(node_t));
        cx_free(J->path, OPT_NUM * sizeof(match_t));
        cx_free(J->path_at, OPT_NUM * sizeof(cx_u32));
    }
    J->hash = J->chain = J->hash3 = 0;
    J->hash_n = J->chain_n = 0;
    J->ss.seq = 0;
    J->ss.lit = 0;
    J->blk = 0;
    J->split = 0;
    J->htmp = 0;
    J->mt = J->path = 0;
    J->opt = 0;
    J->st = 0;
    J->path_at = 0;
}

static void job_ready(job_t* J) {
    if (!J->ss.seq) J->ss.seq = (seq_t*)cx_alloc(SEQ_MAX * sizeof(seq_t));
    if (!J->ss.lit) J->ss.lit = (cx_u8*)cx_alloc(BLOCK_MAX + 32);
    if (!J->blk) J->blk = (cx_u8*)cx_alloc(BLOCK_ROOM);
    if (!J->split) J->split = (cx_u32*)cx_alloc(SPLIT_BYTES);
    if (!J->htmp) J->htmp = (cx_u8*)cx_alloc(4 * HUF_ROOM);
}

/* The most a frame of n bytes takes: its header and checksum, and each block stored. Blocks split
 * into parts of at least 1 KiB, so there are at most n / 1024 + n / BLOCK_MAX + 1 of them, whose
 * headers take less than n / 256 + 3. */
cx_u64 cx_zse_bound(cx_u64 n) {
    return n + (n >> 8) + 3 + 18 + 4 + 8;
}

static cx_u64 phys_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) return m.ullTotalPhys;
    return (cx_u64)8 << 30;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0) return (cx_u64)pages * (cx_u64)size;
    return (cx_u64)8 << 30;
#endif
}

/* What a job made. */
typedef struct {
    cx_u8* out;
    cx_usize n, cap;
} made_t;

typedef struct {
    const cx_u8* in;
    cx_u64 n, each, reach;
    cx_u64 jobs, groups;
    cp_t p;
    made_t* made;
    cx_u8* direct; /* where the blocks go, for jobs that run one at a time */
    cx_usize direct_cap;
    cx_usize direct_n; /* how much the jobs before have written */
    /* With a dictionary, the first job's text after as much of the dictionary's content as the
     * window holds, first_pre bytes, so its matches can reach back into it. */
    cx_u8* first;
    cx_u32 first_pre;
    cx_usize first_cap; /* 0 if first is in the working copy */
    const cx_u8* dparams;
    cx_u64 dkey;
    digest_t* dg; /* the dictionary's kept tables for the first job, held, or 0 */
    work_t* dw;   /* and a working copy of them, or 0 */
} frame_t;

#define START_OF(p) (((cx_u32)1 << (p).wlog) + 1)

static void job_place(frame_t* F, job_t* J, cx_u64 k) {
    const cx_u64 from = k * F->each, to = from + F->each < F->n ? from + F->each : F->n;
    const cx_u64 pre = from > F->reach ? from - F->reach : 0;
    J->p = F->p;
    J->start = START_OF(F->p);
    J->wsize = (cx_u32)1 << F->p.wlog;
    J->base = F->in + pre - J->start;
    J->from = J->start + (cx_u32)(from - pre);
    J->to = J->start + (cx_u32)(to - pre);
    J->last = k + 1 == F->jobs;
    J->first = k == 0;
    J->dparams = k == 0 ? F->dparams : 0;
    J->dkey = k == 0 ? F->dkey : 0;
    J->dg = k == 0 ? F->dg : 0;
    J->dw = k == 0 ? F->dw : 0;
    J->dict = k == 0 && F->first;
    if (k == 0 && F->first) {
        J->base = F->first - J->start;
        J->from = J->start + F->first_pre;
        J->to = J->from + (cx_u32)to;
    }
    J->lim = J->to;
    J->cut = 0;
}

static void group_body(void* v, cx_u64 begin, cx_u64 end) {
    frame_t* F = (frame_t*)v;
    for (cx_u64 g = begin; g < end; g++) {
        job_t* J = (job_t*)cx_alloc(sizeof(job_t));
        memset(J, 0, __builtin_offsetof(job_t, llc));
        job_ready(J);
        for (cx_u64 k = g; k < F->jobs; k += F->groups) {
            made_t* slot = &F->made[k];
            job_place(F, J, k);
            /* Jobs that run one at a time, in order, write in their place in the frame, which saves
             * a buffer for each and copying out of it. One goes in a buffer of its own only if the
             * room left is less than its blocks could take, stored, as for bytes that don't
             * compress; those that come after it still know their place. */
            const cx_usize len = J->to - J->from, most = len + (len >> 8) + 3;
            const int placed = F->direct && F->direct_cap - F->direct_n >= most;
            if (placed) {
                J->out = F->direct + F->direct_n;
                J->out_cap = F->direct_cap - F->direct_n;
            } else {
                J->out_cap = (cx_usize)cx_zse_bound(len);
                J->out = (cx_u8*)cx_alloc(J->out_cap);
            }
            job_run(J);
            slot->out = placed ? 0 : J->out;
            slot->n = J->out_n;
            slot->cap = placed ? 0 : J->out_cap;
            if (F->direct) F->direct_n += J->out_n;
        }
        job_free(J);
        cx_free(J, sizeof(job_t));
    }
}

/* Writes the frame for in[0..n) at the given level into out, which has cx_zse_bound(n) bytes,
 * with a dictionary's content[0..content_n) as the text before it, its ID if that isn't 0, and if
 * params isn't 0 its tables: gives its size. flags: 1 for a checksum, which std.zstd appends. */
static cx_u64 frame_write(const cx_u8* in, cx_u64 n, cx_u8* out, cx_i32 level, cx_u32 flags, const cx_u8* content, cx_u64 content_n, cx_u32 dict_id,
                          const cx_u8* params, cx_u64 key) {
    if (!__atomic_load_n(&tables_ready, __ATOMIC_ACQUIRE)) tables_init();
    frame_t F;
    memset(&F, 0, sizeof F);
    F.in = in;
    F.n = n;
    /* With a dictionary the settings are for it and the input together, as libzstd picks them, so
     * the window takes in what it can of both. */
    F.p = params_for(level, n + content_n);
    F.each = job_size(&F.p);
    F.reach = job_reach(&F.p);
    F.jobs = n ? (n + F.each - 1) / F.each : 1;
    F.dparams = params;
    F.dkey = key;
    if (content_n) {
        const cx_u64 w = (cx_u64)1 << F.p.wlog, len = n < F.each ? n : F.each;
        F.first_pre = (cx_u32)(content_n < w ? content_n : w);
        const cx_u8* const tail = content + content_n - F.first_pre;
        /* The content's tables kept for these settings, if they are, in a working copy, after
         * whose content a small first job's text goes. */
        F.dg = digest_find(&F.p, F.first_pre, tail, key, &F.dw);
        if (F.dw && len <= WORK_TEXT) {
            F.first = F.dw->text;
        } else {
            F.first_cap = F.first_pre + (cx_usize)len;
            F.first = (cx_u8*)cx_alloc(F.first_cap ? F.first_cap : 1);
            memcpy(F.first, tail, F.first_pre);
        }
        if (len) memcpy(F.first + F.first_pre, in, (cx_usize)len);
    }
    /* The frame's header: the dictionary's ID if it has one, the size, and a window unless it's
     * one segment. */
    cx_u8* op = out;
    st32(op, MAGIC);
    op += 4;
    const int single = n <= ((cx_u64)1 << F.p.wlog);
    cx_u32 fcs;
    if (single) fcs = n < 256 ? 0 : n < 65536 + 256 ? 1 : n < ((cx_u64)1 << 32) ? 2 : 3;
    else fcs = n < 65536 + 256 ? 1 : n < ((cx_u64)1 << 32) ? 2 : 3;
    const cx_u32 did = dict_id == 0 ? 0 : dict_id < 256 ? 1 : dict_id < 65536 ? 2 : 3;
    *op++ = (cx_u8)((fcs << 6) | ((cx_u32)single << 5) | ((flags & 1) << 2) | did);
    if (!single) *op++ = (cx_u8)((F.p.wlog - 10) << 3);
    for (cx_u32 k = 0; k < (did == 3 ? 4u : did); k++) *op++ = (cx_u8)(dict_id >> (8 * k));
    switch (fcs) {
    case 0: *op++ = (cx_u8)n; break;
    case 1:
        st16(op, (cx_u32)(n - 256));
        op += 2;
        break;
    case 2:
        st32(op, (cx_u32)n);
        op += 4;
        break;
    default:
        st64(op, n);
        op += 8;
        break;
    }
    F.made = (made_t*)cx_alloc(F.jobs * sizeof(made_t));
    memset(F.made, 0, F.jobs * sizeof(made_t));
    cx_usize workers = cx_workers();
    cx_u64 groups = F.jobs < workers ? F.jobs : workers;
    if (groups > 1) {
        const cx_u64 one = sizeof(job_t) + ((cx_u64)4 << F.p.hlog) + ((cx_u64)4 << F.p.clog) + 2 * F.each;
        cx_u64 fit = phys_memory() / 4 / one;
        if (fit < 1) fit = 1;
        if (groups > fit) groups = fit;
    }
    F.groups = groups ? groups : 1;
    if (F.groups == 1) {
        F.direct = op;
        F.direct_cap = (cx_usize)(cx_zse_bound(n) - (cx_u64)(op - out));
        group_body(&F, 0, 1);
    } else {
        cx_parallel(F.groups, group_body, &F);
    }
    /* Each job's blocks, after the ones before: those written in place are there already. */
    for (cx_u64 k = 0; k < F.jobs; k++) {
        if (F.made[k].out) {
            memcpy(op, F.made[k].out, F.made[k].n);
            cx_free(F.made[k].out, F.made[k].cap);
        }
        op += F.made[k].n;
    }
    cx_free(F.made, F.jobs * sizeof(made_t));
    if (F.first_cap) cx_free(F.first, F.first_cap);
    return (cx_u64)(op - out);
}

cx_u64 cx_zse_compress(const cx_u8* in, cx_u64 n, cx_u8* out, cx_i32 level, cx_u32 flags) {
    return frame_write(in, n, out, level, flags, 0, 0, 0, 0, 0);
}

/* As cx_zse_compress, with a dictionary: its content[0..content_n), its ID, 0 for none, its tables
 * from calx_zstd.c's cx_zstd_dict_params if has_params is set, and its key from cx_zse_dict_key,
 * or 0 to have its kept tables found by its content. */
cx_u64 cx_zse_compress_dict(const cx_u8* in, cx_u64 n, cx_u8* out, cx_i32 level, cx_u32 flags, const cx_u8* content, cx_u64 content_n, cx_u32 dict_id,
                            const cx_u8* params, cx_u32 has_params, cx_u64 key) {
    return frame_write(in, n, out, level, flags, content, content_n, dict_id, has_params ? params : 0, key);
}

/* A key for a new dictionary, which no other has had. */
cx_u64 cx_zse_dict_key(void) {
    static cx_u64 keys;
    return __atomic_add_fetch(&keys, 1, __ATOMIC_RELAXED);
}
