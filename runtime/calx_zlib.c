/* zlib, gzip and raw deflate streams, behind std.zlib (D118). Linked only into programs that call
 * them. Written from RFC 1950 (zlib), RFC 1951 (deflate) and RFC 1952 (gzip). Vanilla compresses
 * packets over its threshold and region-file chunks with zlib, and NBT files with gzip, all at
 * Java's default level, 6.
 *
 * Inflating takes the whole input at once and writes straight into the caller's list, asking for
 * more room when it runs out. It accepts the streams zlib accepts, which is what Java's Inflater
 * is, and where the output is all the room there is, it stops where zlib stops, so a packet
 * vanilla takes, Calcite takes. Each code of up to 10 bits (8 for distances) is one look-up, with
 * a second table for longer ones. The bits come 56 or more at a time from one 8-byte load, and a
 * match is copied 8 bytes at a time.
 *
 * Deflating takes the whole input too. It finds matches through chains of the earlier positions
 * whose first four bytes hash the same, comparing 8 bytes at a time. At levels 4 to 9 it checks
 * whether the next position has a longer match before taking one, as zlib does. Each block of up
 * to BLOCK_SYMS symbols gets its own Huffman code, the fixed code, or no code, whichever is
 * shortest.
 *
 * Words are read and written little-endian, as every target Calx has is. */
#include "calx_rt.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#define CX_X86 1
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "calx_zlib.c reads and writes words as little-endian"
#endif

static inline cx_u64 load64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return v;
}
static inline cx_u32 load32(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}
static inline void store64(cx_u8* p, cx_u64 v) { memcpy(p, &v, 8); }

/* Runs `init` once, for tables that every thread then only reads. */
static void once(cx_u32* state, void (*init)(void)) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2) return;
    cx_u32 idle = 0;
    if (__atomic_compare_exchange_n(state, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        }
    }
}

/* --- checksums ------------------------------------------------------------------------------ */

/* CRC-32 eight bytes a step: crc_table[k][b] is the CRC of byte b followed by k zero bytes. */
static cx_u32 crc_table[8][256];
static cx_u32 crc_ready;

static void crc_init(void) {
    for (cx_u32 b = 0; b < 256; b++) {
        cx_u32 c = b;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_table[0][b] = c;
    }
    for (cx_u32 b = 0; b < 256; b++)
        for (int k = 1; k < 8; k++) crc_table[k][b] = (crc_table[k - 1][b] >> 8) ^ crc_table[0][crc_table[k - 1][b] & 0xff];
}

cx_u32 cx_zlib_crc32(cx_u32 crc, const cx_u8* p, cx_usize n) {
    once(&crc_ready, crc_init);
    crc = ~crc;
    while (n >= 8) {
        cx_u64 v = load64(p) ^ crc;
        crc = crc_table[7][v & 0xff] ^ crc_table[6][v >> 8 & 0xff] ^ crc_table[5][v >> 16 & 0xff] ^ crc_table[4][v >> 24 & 0xff] ^
              crc_table[3][v >> 32 & 0xff] ^ crc_table[2][v >> 40 & 0xff] ^ crc_table[1][v >> 48 & 0xff] ^ crc_table[0][v >> 56];
        p += 8;
        n -= 8;
    }
    while (n--) crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 0xff];
    return ~crc;
}

#ifdef CX_X86
/* Whether the processor has AVX2 and the system saves its registers: 1 no, 2 yes, 0 not yet
 * asked. Asking twice gives the same answer, so threads may race to ask. */
static cx_u32 avx2_state;

static int avx2(void) {
    cx_u32 s = __atomic_load_n(&avx2_state, __ATOMIC_RELAXED);
    if (s) return s == 2;
    unsigned a, b, c, d, lo, hi;
    int yes = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) && c >> 27 & 1 && c >> 28 & 1) {
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if ((lo & 6) == 6 && __get_cpuid_count(7, 0, &a, &b, &c, &d)) yes = b >> 5 & 1;
    }
    __atomic_store_n(&avx2_state, yes ? 2u : 1u, __ATOMIC_RELAXED);
    return yes;
}

/* Adler-32 32 bytes a step. Over a block of bytes x0..x31, a grows by their sum and b by 32 times
 * a as it was, plus 32 x0 + 31 x1 + ... + x31. The sums of the bytes go in 64-bit lanes, the
 * weighted sums in 32-bit lanes, and the sum of a before each block in a third, all added up
 * once per 5536 bytes, which is as far as the 32-bit lanes are safe. */
__attribute__((target("avx2"))) static cx_u32 adler32_avx2(cx_u32 a, cx_u32 b, const cx_u8* p, cx_usize n, cx_usize* left) {
    const __m256i weights = _mm256_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11,
                                             10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m256i ones = _mm256_set1_epi16(1), zero = _mm256_setzero_si256();
    while (n >= 32) {
        cx_usize blocks = n / 32 < 173 ? n / 32 : 173;
        n -= blocks * 32;
        __m256i sum = zero, before = zero, weighted = zero;
        for (cx_usize k = 0; k < blocks; k++) {
            __m256i v = _mm256_loadu_si256((const __m256i*)p);
            before = _mm256_add_epi64(before, sum);
            sum = _mm256_add_epi64(sum, _mm256_sad_epu8(v, zero));
            weighted = _mm256_add_epi32(weighted, _mm256_madd_epi16(_mm256_maddubs_epi16(v, weights), ones));
            p += 32;
        }
        cx_u64 lanes[4], s = 0, sb = 0, w = 0;
        cx_u32 wl[8];
        _mm256_storeu_si256((__m256i*)lanes, sum);
        for (int i = 0; i < 4; i++) s += lanes[i];
        _mm256_storeu_si256((__m256i*)lanes, before);
        for (int i = 0; i < 4; i++) sb += lanes[i];
        _mm256_storeu_si256((__m256i*)wl, weighted);
        for (int i = 0; i < 8; i++) w += wl[i];
        b = (cx_u32)((b + (cx_u64)a * 32 * blocks + 32 * sb + w) % 65521);
        a = (cx_u32)((a + s) % 65521);
    }
    *left = n;
    return b << 16 | a;
}
#endif

/* Adler-32. 5552 bytes is the most that can be summed before the 32-bit sums could overflow, so
 * the remainders are taken once per 5552 bytes. */
cx_u32 cx_zlib_adler32(cx_u32 adler, const cx_u8* p, cx_usize n) {
    cx_u32 a = adler & 0xffff, b = adler >> 16;
#ifdef CX_X86
    if (n >= 64 && avx2()) {
        cx_usize left;
        cx_u32 r = adler32_avx2(a, b, p, n, &left);
        p += n - left, n = left;
        a = r & 0xffff, b = r >> 16;
    }
#endif
    while (n) {
        cx_usize k = n < 5552 ? n : 5552;
        n -= k;
        while (k >= 8) {
            a += p[0], b += a;
            a += p[1], b += a;
            a += p[2], b += a;
            a += p[3], b += a;
            a += p[4], b += a;
            a += p[5], b += a;
            a += p[6], b += a;
            a += p[7], b += a;
            p += 8;
            k -= 8;
        }
        while (k--) a += *p++, b += a;
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* --- the tables deflate defines --------------------------------------------------------------- */

/* Length codes 257 to 285 and distance codes 0 to 29: the first value of each, and how many
 * extra bits follow it. Worked out from RFC 1951's rule rather than copied out. */
static cx_u16 len_base[29], dist_base[30];
static cx_u8 len_extra[29], dist_extra[30];
/* For deflating: the length code (less 257) for each length less 3, and the distance code for
 * each distance less 1, as dist_code reads it. */
static cx_u8 len_code[256], dist_codes[512];

/* The order the code length code's lengths come in. */
static const cx_u8 cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static inline cx_u32 dist_code(cx_u32 d) { return d <= 256 ? dist_codes[d - 1] : dist_codes[256 + ((d - 1) >> 7)]; }

/* --- inflating ------------------------------------------------------------------------------ */

#define LBITS 10
#define DBITS 8
/* Enough for any code zlib accepts: a complete code has a second-level table only under a prefix
 * with two or more codes below it, so at most 143 for lengths and 15 for distances, each at most
 * 2^(15 - root) entries. */
#define LT_SIZE (1024 + 143 * 32)
#define DT_SIZE (256 + 15 * 128)

/* A table entry. Bits 0 to 3 are how many bits the code takes, and bits 4 to 7 the extra bits
 * after it, or for a pointer to a second table, that table's bits. Bits 16 to 30 are the value:
 * the literal, the first length or distance of the code, or where the second table starts. */
#define F_EOB (1u << 8)
#define F_SUB (1u << 9)
#define F_BAD (1u << 10)
#define F_LIT (1u << 31)

enum { K_CODES, K_LENS, K_DISTS };

static cx_u32 fixed_lt[1 << LBITS], fixed_dt[1 << DBITS];
/* For deflating with the fixed code: each symbol's code, reversed so it goes out first bit first,
 * and its length. */
static cx_u16 fixed_lcode[288], fixed_dcode[30];
static cx_u8 fixed_llen[288], fixed_dlen[30];
static cx_u32 tables_ready;

static void make_codes(const cx_u8* lens, int n, cx_u16* codes);

static cx_u32 reverse(cx_u32 code, int len) {
    cx_u32 r = 0;
    while (len--) {
        r = r << 1 | (code & 1);
        code >>= 1;
    }
    return r;
}

static cx_u32 symbol_entry(int kind, int s) {
    if (kind == K_CODES) return (cx_u32)s << 16;
    if (kind == K_LENS) {
        if (s < 256) return F_LIT | (cx_u32)s << 16;
        if (s == 256) return F_EOB;
        if (s < 286) return (cx_u32)len_base[s - 257] << 16 | (cx_u32)len_extra[s - 257] << 4;
        return F_BAD;
    }
    if (s < 30) return (cx_u32)dist_base[s] << 16 | (cx_u32)dist_extra[s] << 4;
    return F_BAD;
}

/* Fills `t` for the canonical code with these lengths, looking `root` bits up at once. Returns
 * whether zlib accepts the lengths. No code may be over-subscribed. One that doesn't use every
 * bit pattern is accepted only if its longest code is one bit, for lengths or distances. A code
 * with no symbols at all is accepted too: for distances it's a block with no matches, and for
 * the code length code zlib reads every pattern as a length of 0 in one bit, so it's that here,
 * and the block fails later for having no end. Patterns no code has are F_BAD. */
static int build(cx_u32* t, int root, const cx_u8* lens, int n, int kind) {
    cx_u16 count[16] = {0};
    for (int s = 0; s < n; s++) count[lens[s]]++;
    int max = 15;
    while (max > 0 && count[max] == 0) max--;
    cx_u32 size = 1u << root;
    if (max == 0) {
        for (cx_u32 i = 0; i < size; i++) t[i] = kind == K_CODES ? 1 : F_BAD | 1;
        return kind != K_LENS;
    }
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left = (left << 1) - count[len];
        if (left < 0) return 0;
    }
    if (left > 0 && (kind == K_CODES || max != 1)) return 0;
    cx_u16 offs[16], sorted[288];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
    for (int s = 0; s < n; s++)
        if (lens[s]) sorted[offs[lens[s]]++] = (cx_u16)s;
    int total = n - count[0];
    cx_u32 code = 0, next = size, prefix = ~0u, sub = 0, sub_bits = 0, cur = 2;
    int prev = lens[sorted[0]], i = 0;
    /* Codes up to `root` bits: each length's go into a table of 2^length entries, which is then
     * doubled for the next length, so a short code is copied rather than stored once for each
     * entry it fills. What no code fills stays F_BAD. */
    t[0] = t[1] = F_BAD | 1;
    for (int l = 1; l <= root; l++) {
        if (l > 1) {
            memcpy(t + cur, t, cur * sizeof t[0]);
            cur <<= 1;
        }
        for (; i < total && lens[sorted[i]] == l; i++) {
            if (i > 0) code = (code + 1) << (l - prev);
            prev = l;
            t[reverse(code, l)] = symbol_entry(kind, sorted[i]) | (cx_u32)l;
        }
    }
    for (; i < total; i++) {
        int l = lens[sorted[i]];
        if (i > 0) code = (code + 1) << (l - prev);
        prev = l;
        cx_u32 r = reverse(code, l), e = symbol_entry(kind, sorted[i]) | (cx_u32)l;
        if ((r & (size - 1)) != prefix) {
            /* The codes under this prefix come next in order: the table must reach the longest. */
            int longest = l, lj = l;
            cx_u32 c = code;
            for (int j = i + 1; j < total; j++) {
                int len = lens[sorted[j]];
                c = (c + 1) << (len - lj);
                lj = len;
                if (c >> (len - root) != code >> (l - root)) break;
                longest = len;
            }
            prefix = r & (size - 1);
            sub = next;
            sub_bits = (cx_u32)(longest - root);
            next += 1u << sub_bits;
            t[prefix] = F_SUB | sub << 16 | sub_bits << 4 | (cx_u32)root;
        }
        for (cx_u32 k = r >> root; k < 1u << sub_bits; k += 1u << (l - root)) t[sub + k] = e;
    }
    return 1;
}

static void tables_init(void) {
    cx_u32 b = 3;
    for (int c = 0; c < 28; c++) {
        len_extra[c] = (cx_u8)(c < 8 ? 0 : (c - 4) / 4);
        len_base[c] = (cx_u16)b;
        for (cx_u32 k = 0; k < 1u << len_extra[c]; k++) len_code[b - 3 + k] = (cx_u8)c;
        b += 1u << len_extra[c];
    }
    len_base[28] = 258, len_extra[28] = 0, len_code[255] = 28;
    b = 1;
    for (int c = 0; c < 30; c++) {
        dist_extra[c] = (cx_u8)(c < 4 ? 0 : c / 2 - 1);
        dist_base[c] = (cx_u16)b;
        for (cx_u32 k = 0; k < 1u << dist_extra[c]; k++) {
            cx_u32 d = b - 1 + k;
            if (d < 256) dist_codes[d] = (cx_u8)c;
            else dist_codes[256 + (d >> 7)] = (cx_u8)c;
        }
        b += 1u << dist_extra[c];
    }
    cx_u8 lens[288];
    for (int s = 0; s < 288; s++) lens[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    build(fixed_lt, LBITS, lens, 288, K_LENS);
    for (int s = 0; s < 288; s++) fixed_llen[s] = lens[s];
    make_codes(lens, 288, fixed_lcode);
    for (int s = 0; s < 32; s++) lens[s] = 5;
    build(fixed_dt, DBITS, lens, 32, K_DISTS);
    for (int s = 0; s < 30; s++) fixed_dlen[s] = 5, fixed_dcode[s] = (cx_u16)reverse((cx_u32)s, 5);
}

enum { S_HEADER, S_BLOCK, S_STORED, S_CODES, S_TRAILER, S_DONE };
enum { F_ZLIB, F_GZIP, F_RAW };

typedef struct {
    cx_u32 lt[LT_SIZE];
    cx_u32 dt[DT_SIZE];
    cx_u64 bits;        /* bits read from the input and not yet used, first bit lowest */
    cx_usize at;        /* the next input byte to read */
    cx_usize start;     /* where this stream's output starts in the list: no distance reaches before */
    cx_usize len;       /* where the next byte goes */
    cx_usize checked;   /* the output before this is in `check` */
    cx_usize stored;    /* bytes left in a stored block */
    cx_u32 nbits;       /* how many of `bits` are the input's */
    cx_u32 overrun;     /* zero bytes put in `bits` past the end of the input */
    cx_u32 format, stage, last, fixed;
    cx_u32 check;       /* the Adler-32 or CRC-32 of the output so far */
    cx_u32 members;     /* gzip members finished */
} inflater;

_Static_assert(sizeof(inflater) <= CX_INFLATE_CTX, "the context fits what std.zlib makes room for");

void cx_inflate_start(cx_u8* ctx, cx_u32 format, cx_usize start) {
    once(&tables_ready, tables_init);
    inflater* z = (inflater*)ctx;
    z->bits = 0, z->nbits = 0, z->overrun = 0, z->at = 0;
    z->start = z->len = z->checked = start;
    z->stored = 0;
    z->format = format, z->stage = S_HEADER, z->last = 0, z->fixed = 0;
    z->check = format == F_ZLIB ? 1 : 0;
    z->members = 0;
}

cx_usize cx_inflate_len(const cx_u8* ctx) { return ((const inflater*)ctx)->len; }
cx_usize cx_inflate_used(const cx_u8* ctx) { return ((const inflater*)ctx)->at; }

/* A gzip member's header at in[*at]: 0 if it is one, and *at moves past it; -1 if it isn't one;
 * -2 if the input ends inside it. The reserved flags are ignored and the header's own CRC is
 * checked, as Java does. */
static int gzip_header(const cx_u8* in, cx_usize n, cx_usize* at) {
    cx_usize p = *at;
    if (n - p >= 1 && in[p] != 0x1f) return -1;
    if (n - p >= 2 && in[p + 1] != 0x8b) return -1;
    if (n - p >= 3 && in[p + 2] != 8) return -1;
    if (n - p < 10) return -2;
    cx_u32 flags = in[p + 3];
    p += 10;
    if (flags & 4) {
        if (n - p < 2) return -2;
        cx_usize x = (cx_usize)in[p] | (cx_usize)in[p + 1] << 8;
        p += 2;
        if (n - p < x) return -2;
        p += x;
    }
    for (cx_u32 f = 8; f <= 16; f <<= 1)
        if (flags & f) {
            const cx_u8* z = memchr(in + p, 0, n - p);
            if (!z) return -2;
            p = (cx_usize)(z - in) + 1;
        }
    if (flags & 2) {
        if (n - p < 2) return -2;
        cx_u32 c = cx_zlib_crc32(0, in + *at, p - *at);
        if ((cx_u32)in[p] != (c & 0xff) || (cx_u32)in[p + 1] != (c >> 8 & 0xff)) return -1;
        p += 2;
    }
    *at = p;
    return 0;
}

/* Copies a match of `len` bytes from `dist` back. It may write up to 15 bytes past the end, which
 * the fast loop leaves room for, and which later output writes over. */
static inline void copy_match(cx_u8* op, cx_u32 dist, cx_u32 len) {
    const cx_u8* src = op - dist;
    cx_u8* end = op + len;
    if (dist >= 16) {
        do {
            memcpy(op, src, 16);
            op += 16, src += 16;
        } while (op < end);
    } else if (dist >= 8) {
        do {
            store64(op, load64(src));
            op += 8, src += 8;
        } while (op < end);
    } else if (dist == 1) {
        cx_u64 v = 0x0101010101010101ull * src[0];
        do {
            store64(op, v);
            op += 8;
        } while (op < end);
    } else {
        /* The first 8 bytes one at a time; after that the pattern repeats every multiple of
         * `dist` that's 8 or more, so 8 bytes at a time can be copied from that far back. */
        for (int i = 0; i < 8; i++) op[i] = src[i];
        static const cx_u8 period[8] = {0, 0, 8, 9, 8, 10, 12, 14};
        cx_u32 p = period[dist];
        for (cx_u32 i = 8; i < len; i += 8) store64(op + i, load64(op + i - p));
    }
}

#define REFILL()                                                                \
    do {                                                                        \
        if (n - at >= 8) {                                                      \
            bits |= load64(in + at) << nbits;                                   \
            at += (63 - nbits) >> 3;                                            \
            nbits |= 56;                                                        \
        } else {                                                                \
            while (nbits <= 56) {                                               \
                if (at < n) bits |= (cx_u64)in[at++] << nbits;                  \
                else overrun++;                                                 \
                nbits += 8;                                                     \
            }                                                                   \
        }                                                                       \
    } while (0)
#define DROP(k) (bits >>= (k), nbits -= (k))
/* Whether some of the bits used were the zeros past the end of the input. */
#define PAST_END() (overrun * 8 > nbits)
#define FAIL(code) \
    do {           \
        st = code; \
        goto leave; \
    } while (0)

/* Gives back the whole bytes left in `bits`, after dropping what's left of the current byte, so
 * the input can be read a byte at a time from `at`. */
#define UNREAD()                          \
    do {                                  \
        DROP(nbits & 7);                  \
        if (PAST_END()) FAIL(-2);         \
        at -= (nbits >> 3) - overrun;     \
        bits = 0, nbits = 0, overrun = 0; \
    } while (0)

/* Inflates from in[0..n), the same bytes on every call, into out[len..end), where `out` is the
 * list's memory, which may have moved since the last call. Returns 0 when the stream has ended,
 * 1 when it needs more room, 2 when it stopped at `end` because `final` says there's no more
 * room, and -1 for a stream zlib rejects, -2 for one that's cut off, -3 for a wrong checksum,
 * -4 for one that needs a preset dictionary.
 * After 1 the next call carries on; after anything else, there's no next call. */
cx_i32 cx_inflate(cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize end, cx_bool final) {
    inflater* z = (inflater*)ctx;
    cx_u64 bits = z->bits;
    cx_u32 nbits = z->nbits, overrun = z->overrun;
    cx_usize at = z->at;
    cx_u8 *op = out + z->len, *oend = out + end;
    const cx_u8* ostart = out + z->start;
    cx_i32 st = 0;
    for (;;) switch (z->stage) {
            case S_HEADER:
                if (z->format == F_ZLIB) {
                    if (n - at < 2) FAIL(-2);
                    cx_u32 cmf = in[at], flg = in[at + 1];
                    if ((cmf << 8 | flg) % 31 || (cmf & 15) != 8 || cmf >> 4 > 7) FAIL(-1);
                    /* A preset dictionary: zlib reads its id, then waits to be given it. */
                    if (flg & 0x20) {
                        if (n - at < 6) FAIL(-2);
                        at += 6;
                        FAIL(-4);
                    }
                    at += 2;
                } else if (z->format == F_GZIP) {
                    int h = gzip_header(in, n, &at);
                    if (h) FAIL(h);
                }
                z->stage = S_BLOCK;
                break;
            case S_BLOCK: {
                REFILL();
                z->last = (cx_u32)bits & 1;
                cx_u32 type = (cx_u32)bits >> 1 & 3;
                DROP(3);
                if (PAST_END()) FAIL(-2);
                if (type == 0) {
                    DROP(nbits & 7);
                    REFILL();
                    cx_u32 len = (cx_u32)bits & 0xffff, nlen = (cx_u32)(bits >> 16) & 0xffff;
                    DROP(32);
                    if (PAST_END()) FAIL(-2);
                    if (len != (~nlen & 0xffff)) FAIL(-1);
                    UNREAD();
                    z->stored = len;
                    z->stage = S_STORED;
                } else if (type == 1) {
                    z->fixed = 1;
                    z->stage = S_CODES;
                } else if (type == 2) {
                    cx_u32 nlen = ((cx_u32)bits & 31) + 257, ndist = ((cx_u32)bits >> 5 & 31) + 1, ncode = ((cx_u32)bits >> 10 & 15) + 4;
                    DROP(14);
                    if (PAST_END()) FAIL(-2);
                    if (nlen > 286 || ndist > 30) FAIL(-1);
                    cx_u8 cl[19] = {0};
                    for (cx_u32 i = 0; i < ncode; i++) {
                        if (nbits < 3) REFILL();
                        cl[cl_order[i]] = (cx_u8)(bits & 7);
                        DROP(3);
                    }
                    if (PAST_END()) FAIL(-2);
                    cx_u32 ct[128];
                    if (!build(ct, 7, cl, 19, K_CODES)) FAIL(-1);
                    cx_u8 lens[316];
                    cx_u32 i = 0, total = nlen + ndist;
                    while (i < total) {
                        if (nbits < 14) REFILL();
                        cx_u32 e = ct[bits & 127];
                        DROP(e & 15);
                        cx_u32 sym = e >> 16 & 0x7fff, rep;
                        cx_u8 v = 0;
                        if (sym < 16) {
                            if (PAST_END()) FAIL(-2);
                            lens[i++] = (cx_u8)sym;
                            continue;
                        }
                        if (sym == 16) {
                            rep = 3 + ((cx_u32)bits & 3);
                            DROP(2);
                        } else if (sym == 17) {
                            rep = 3 + ((cx_u32)bits & 7);
                            DROP(3);
                        } else {
                            rep = 11 + ((cx_u32)bits & 127);
                            DROP(7);
                        }
                        if (PAST_END()) FAIL(-2);
                        if (sym == 16) {
                            if (i == 0) FAIL(-1);
                            v = lens[i - 1];
                        }
                        if (rep > total - i) FAIL(-1);
                        memset(lens + i, v, rep);
                        i += rep;
                    }
                    if (lens[256] == 0) FAIL(-1);
                    if (!build(z->lt, LBITS, lens, (int)nlen, K_LENS) || !build(z->dt, DBITS, lens + nlen, (int)ndist, K_DISTS)) FAIL(-1);
                    z->fixed = 0;
                    z->stage = S_CODES;
                } else {
                    FAIL(-1);
                }
                break;
            }
            case S_STORED: {
                if (z->stored == 0) {
                    z->stage = z->last ? S_TRAILER : S_BLOCK;
                    break;
                }
                cx_usize k = z->stored, room = (cx_usize)(oend - op);
                if (k > n - at) k = n - at;
                if (k > room) k = room;
                memcpy(op, in + at, k);
                op += k, at += k, z->stored -= k;
                if (z->stored == 0) break;
                if (op == oend) FAIL(final ? 2 : 1);
                FAIL(-2);
            }
            case S_CODES: {
                const cx_u32* lt = z->fixed ? fixed_lt : z->lt;
                const cx_u32* dt = z->fixed ? fixed_dt : z->dt;
                for (;;) {
                    /* Fast: with 16 bytes of input and 274 of room, no check is needed until the
                     * next symbol. */
                    while (n - at >= 16 && (cx_usize)(oend - op) >= 274) {
                        bits |= load64(in + at) << nbits;
                        at += (63 - nbits) >> 3;
                        nbits |= 56;
                        cx_u32 e = lt[bits & ((1u << LBITS) - 1)];
                        if (e & F_SUB) e = lt[(e >> 16 & 0x7fff) + ((cx_u32)(bits >> LBITS) & ((1u << (e >> 4 & 15)) - 1))];
                        DROP(e & 15);
                        if (e & F_LIT) {
                            *op++ = (cx_u8)(e >> 16);
                            /* Two more literals fit in the 41 bits or more that are left. */
                            e = lt[bits & ((1u << LBITS) - 1)];
                            if (e & F_SUB) e = lt[(e >> 16 & 0x7fff) + ((cx_u32)(bits >> LBITS) & ((1u << (e >> 4 & 15)) - 1))];
                            if (!(e & F_LIT)) continue;
                            DROP(e & 15);
                            *op++ = (cx_u8)(e >> 16);
                            e = lt[bits & ((1u << LBITS) - 1)];
                            if (e & F_SUB) e = lt[(e >> 16 & 0x7fff) + ((cx_u32)(bits >> LBITS) & ((1u << (e >> 4 & 15)) - 1))];
                            if (!(e & F_LIT)) continue;
                            DROP(e & 15);
                            *op++ = (cx_u8)(e >> 16);
                            continue;
                        }
                        if (e & (F_EOB | F_BAD)) {
                            if (e & F_BAD) FAIL(-1);
                            goto block_end;
                        }
                        cx_u32 x = e >> 4 & 15;
                        cx_u32 len = (e >> 16 & 0x7fff) + ((cx_u32)bits & ((1u << x) - 1));
                        DROP(x);
                        cx_u32 d = dt[bits & ((1u << DBITS) - 1)];
                        if (d & F_SUB) d = dt[(d >> 16 & 0x7fff) + ((cx_u32)(bits >> DBITS) & ((1u << (d >> 4 & 15)) - 1))];
                        if (d & F_BAD) FAIL(-1);
                        DROP(d & 15);
                        x = d >> 4 & 15;
                        cx_u32 dist = (d >> 16 & 0x7fff) + ((cx_u32)bits & ((1u << x) - 1));
                        DROP(x);
                        if (dist > (cx_usize)(op - ostart)) FAIL(-1);
                        copy_match(op, dist, len);
                        op += len;
                    }
                    /* Slow: one symbol, checking everything, and putting the bits back if the
                     * symbol's output doesn't fit and more room is coming. */
                    cx_u64 sbits = bits;
                    cx_u32 snbits = nbits, sover = overrun;
                    cx_usize sat = at;
                    REFILL();
                    cx_u32 e = lt[bits & ((1u << LBITS) - 1)];
                    if (e & F_SUB) e = lt[(e >> 16 & 0x7fff) + ((cx_u32)(bits >> LBITS) & ((1u << (e >> 4 & 15)) - 1))];
                    DROP(e & 15);
                    if (PAST_END()) FAIL(-2);
                    if (e & F_LIT) {
                        if (op == oend) {
                            bits = sbits, nbits = snbits, overrun = sover, at = sat;
                            FAIL(final ? 2 : 1);
                        }
                        *op++ = (cx_u8)(e >> 16);
                        continue;
                    }
                    if (e & F_EOB) goto block_end;
                    if (e & F_BAD) FAIL(-1);
                    cx_u32 x = e >> 4 & 15;
                    cx_u32 len = (e >> 16 & 0x7fff) + ((cx_u32)bits & ((1u << x) - 1));
                    DROP(x);
                    cx_u32 d = dt[bits & ((1u << DBITS) - 1)];
                    if (d & F_SUB) d = dt[(d >> 16 & 0x7fff) + ((cx_u32)(bits >> DBITS) & ((1u << (d >> 4 & 15)) - 1))];
                    DROP(d & 15);
                    if (PAST_END()) FAIL(-2);
                    if (d & F_BAD) FAIL(-1);
                    x = d >> 4 & 15;
                    cx_u32 dist = (d >> 16 & 0x7fff) + ((cx_u32)bits & ((1u << x) - 1));
                    DROP(x);
                    if (PAST_END()) FAIL(-2);
                    cx_usize room = (cx_usize)(oend - op);
                    /* zlib checks the distance only when it has room to copy. */
                    if (room == 0) {
                        bits = sbits, nbits = snbits, overrun = sover, at = sat;
                        FAIL(final ? 2 : 1);
                    }
                    if (dist > (cx_usize)(op - ostart)) FAIL(-1);
                    if (len > room) {
                        if (!final) {
                            bits = sbits, nbits = snbits, overrun = sover, at = sat;
                            FAIL(1);
                        }
                        const cx_u8* src = op - dist;
                        for (cx_usize i = 0; i < room; i++) op[i] = src[i];
                        op = oend;
                        FAIL(2);
                    }
                    const cx_u8* src = op - dist;
                    for (cx_u32 i = 0; i < len; i++) op[i] = src[i];
                    op += len;
                }
            block_end:
                z->stage = z->last ? S_TRAILER : S_BLOCK;
                break;
            }
            case S_TRAILER: {
                UNREAD();
                cx_usize from = z->checked, to = (cx_usize)(op - out);
                z->check = z->format == F_GZIP ? cx_zlib_crc32(z->check, out + from, to - from) : cx_zlib_adler32(z->check, out + from, to - from);
                z->checked = to;
                if (z->format == F_ZLIB) {
                    if (n - at < 4) FAIL(-2);
                    cx_u32 want = (cx_u32)in[at] << 24 | (cx_u32)in[at + 1] << 16 | (cx_u32)in[at + 2] << 8 | in[at + 3];
                    if (want != z->check) FAIL(-3);
                    at += 4;
                } else if (z->format == F_GZIP) {
                    if (n - at < 8) FAIL(-2);
                    if (load32(in + at) != z->check || load32(in + at + 4) != (cx_u32)(to - z->start)) FAIL(-3);
                    at += 8;
                    /* Another member may follow, as Java reads on into one. Anything else after
                     * the first member is ignored, as Java ignores it. */
                    cx_usize next = at;
                    if (n - at > 0 && gzip_header(in, n, &next) == 0) {
                        at = next;
                        z->members++;
                        z->start = to;
                        z->check = 0;
                        ostart = out + to;
                        z->stage = S_BLOCK;
                        break;
                    }
                }
                z->stage = S_DONE;
                break;
            }
            default:
                st = 0;
                goto leave;
        }
leave:
    if (z->format != F_RAW && z->stage != S_DONE) {
        cx_usize from = z->checked, to = (cx_usize)(op - out);
        z->check = z->format == F_GZIP ? cx_zlib_crc32(z->check, out + from, to - from) : cx_zlib_adler32(z->check, out + from, to - from);
        z->checked = to;
    }
    z->bits = bits, z->nbits = nbits, z->overrun = overrun, z->at = at;
    z->len = (cx_usize)(op - out);
    return st;
}

/* --- deflating ------------------------------------------------------------------------------ */

#define WSIZE 32768u
#define MIN_MATCH 4
#define MAX_MATCH 258
#define BLOCK_SYMS 16384

/* How hard each level looks: how many earlier positions it tries, the length that's good enough
 * to stop at, whether it waits a position for a longer match (and up to what length it bothers),
 * and the length past which it tries a quarter as many. */
typedef struct {
    cx_u16 chain, nice, lazy, good;
} level_cfg;
static const level_cfg levels[10] = {
    {0, 0, 0, 0},       {2, 16, 0, 0},       {6, 32, 0, 0},       {16, 64, 0, 0},       {16, 32, 8, 4},
    {32, 64, 16, 8},    {128, 128, 16, 8},   {256, 192, 64, 16},  {1024, 258, 128, 32}, {4096, 258, 258, 32},
};

typedef struct {
    cx_u8* p;
    cx_u64 bits;
    cx_u32 n;
} bitout;

/* Adds `k` bits, k at most 32. A full 32 go out as one 8-byte store, which the output leaves
 * room for. */
static inline void put(bitout* w, cx_u64 v, cx_u32 k) {
    w->bits |= v << w->n;
    w->n += k;
    if (w->n >= 32) {
        store64(w->p, w->bits);
        w->p += 4;
        w->bits >>= 32;
        w->n -= 32;
    }
}

/* Writes out the whole bytes, after padding to a byte. */
static void put_align(bitout* w) {
    w->n = (w->n + 7) & ~7u;
    while (w->n) {
        *w->p++ = (cx_u8)w->bits;
        w->bits >>= 8;
        w->n -= 8;
    }
}

typedef struct {
    const cx_u8* in;
    cx_u32 n;
    cx_u32* head;
    cx_u32* prev;
    cx_u32 hbits, wmask;
    cx_u32* syms;     /* a literal's byte, or 1 << 24 | distance << 8 | (length - 3) */
    cx_u32 nsyms;
    cx_u32 block;     /* the first input byte of the current block */
    cx_u32 lfreq[286], dfreq[30];
    bitout w;
} deflater;

static inline cx_u32 hash4(const cx_u8* p, cx_u32 bits) { return (load32(p) * 0x9E3779B1u) >> (32 - bits); }

static inline cx_u32 match_len(const cx_u8* a, const cx_u8* b, cx_u32 max) {
    cx_u32 len = 0;
    while (len + 8 <= max) {
        cx_u64 x = load64(a + len) ^ load64(b + len);
        if (x) return len + (cx_u32)(__builtin_ctzll(x) >> 3);
        len += 8;
    }
    while (len < max && a[len] == b[len]) len++;
    return len;
}

/* The longest match for the position at `pos` longer than `best`, trying up to `chain` earlier
 * positions from `cand` back. Sets *dist when it finds one. */
static inline cx_u32 longest(const deflater* D, cx_u32 pos, cx_u32 cand, cx_u32 best, cx_u32* dist, cx_u32 chain, cx_u32 nice) {
    const cx_u8* cur = D->in + pos;
    cx_u32 max = D->n - pos < MAX_MATCH ? D->n - pos : MAX_MATCH;
    if (nice > max) nice = max;
    if (best >= max) return best;
    cx_u32 limit = pos > WSIZE ? pos - WSIZE : 0;
    while (cand >= limit && chain--) {
        const cx_u8* m = D->in + cand;
        /* The byte at `best` has to match for the match to be longer, and the first four for it
         * to be one at all: both are checked with one load before comparing the rest. */
        if (load32(m + best - 3) == load32(cur + best - 3) && load32(m) == load32(cur)) {
            cx_u32 len = match_len(m, cur, max);
            if (len > best) {
                best = len;
                *dist = pos - cand;
                if (len >= nice) break;
            }
        }
        cx_u32 next = D->prev[cand & D->wmask];
        if (next >= cand) break;
        cand = next;
    }
    return best;
}

static inline void insert(deflater* D, cx_u32 pos) {
    cx_u32 h = hash4(D->in + pos, D->hbits);
    D->prev[pos & D->wmask] = D->head[h];
    D->head[h] = pos;
}

/* Huffman code lengths of at most `limit` bits for the symbols with these frequencies; 0 for a
 * symbol that isn't used. The code is made with two queues: the leaves sorted by frequency, and
 * the inner nodes in the order they're made, which is sorted too. If it comes out too deep, codes
 * past the limit are moved up and others moved down until the lengths fit again. Then the lengths
 * go to the leaves in order, the longest to the rarest. At least two symbols get codes, so the
 * code is complete. */
static void code_lengths(const cx_u32* freq, int n, int limit, cx_u8* lens) {
    cx_u32 leaf[288], tmp[288];
    int m = 0;
    for (int s = 0; s < n; s++) {
        lens[s] = 0;
        if (freq[s]) leaf[m++] = freq[s] << 9 | (cx_u32)s;
    }
    if (m < 2) {
        /* One symbol or none: give two a one-bit code. */
        int a = m ? (int)(leaf[0] & 511) : 0;
        lens[a] = 1;
        lens[a == 0 ? 1 : 0] = 1;
        return;
    }
    /* Sort by frequency, then symbol: a merge sort, bottom up. */
    for (int width = 1; width < m; width *= 2) {
        for (int lo = 0; lo < m; lo += 2 * width) {
            int mid = lo + width < m ? lo + width : m, hi = lo + 2 * width < m ? lo + 2 * width : m;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = leaf[i] <= leaf[j] ? leaf[i++] : leaf[j++];
            while (i < mid) tmp[k++] = leaf[i++];
            while (j < hi) tmp[k++] = leaf[j++];
        }
        memcpy(leaf, tmp, (size_t)m * sizeof leaf[0]);
    }
    /* Nodes 0..m-1 are the leaves, m..2m-2 the inner nodes. */
    cx_u32 weight[576];
    cx_u16 parent[576];
    for (int i = 0; i < m; i++) weight[i] = leaf[i] >> 9;
    int li = 0, ni = m;
    for (int k = m; k < 2 * m - 1; k++) {
        int pick[2];
        for (int t = 0; t < 2; t++) pick[t] = li < m && (ni >= k || weight[li] <= weight[ni]) ? li++ : ni++;
        weight[k] = weight[pick[0]] + weight[pick[1]];
        parent[pick[0]] = parent[pick[1]] = (cx_u16)k;
    }
    cx_u8 depth[576];
    depth[2 * m - 2] = 0;
    cx_u32 count[32] = {0};
    for (int k = 2 * m - 3; k >= 0; k--) {
        depth[k] = depth[parent[k]] + 1;
        if (k < m) count[depth[k] < 31 ? depth[k] : 31]++;
    }
    int max = 31;
    while (count[max] == 0) max--;
    if (max > limit) {
        for (int len = limit + 1; len < 32; len++) count[limit] += count[len], count[len] = 0;
        cx_u32 total = 0;
        for (int len = 1; len <= limit; len++) total += count[len] << (limit - len);
        while (total > 1u << limit) {
            /* A leaf at the limit becomes the sibling of a shorter leaf moved down one: one less
             * unit of the limit's length in all. */
            count[limit]--;
            int len = limit - 1;
            while (count[len] == 0) len--;
            count[len]--;
            count[len + 1] += 2;
            total--;
        }
    }
    int i = 0;
    for (int len = limit; len >= 1; len--)
        for (cx_u32 c = 0; c < count[len]; c++) lens[leaf[i++] & 511] = (cx_u8)len;
}

/* The canonical codes for these lengths, reversed to go out first bit first. */
static void make_codes(const cx_u8* lens, int n, cx_u16* codes) {
    cx_u16 count[16] = {0}, next[16];
    for (int s = 0; s < n; s++) count[lens[s]]++;
    count[0] = 0;
    cx_u32 c = 0;
    for (int len = 1; len < 16; len++) {
        c = (c + count[len - 1]) << 1;
        next[len] = (cx_u16)c;
    }
    for (int s = 0; s < n; s++)
        if (lens[s]) codes[s] = (cx_u16)reverse(next[lens[s]]++, lens[s]);
}

/* The code length code's symbols for the lengths, run-length coded: 16 repeats the last length
 * 3 to 6 times, 17 gives 3 to 10 zeros and 18 gives 11 to 138. Each is the symbol, with the
 * repeat's extra bits above bit 5. Returns how many. */
static int rle_lengths(const cx_u8* lens, int n, cx_u16* out) {
    int k = 0;
    for (int i = 0; i < n;) {
        cx_u8 v = lens[i];
        int run = 1;
        while (i + run < n && lens[i + run] == v) run++;
        i += run;
        if (v == 0) {
            while (run >= 11) {
                int r = run < 138 ? run : 138;
                out[k++] = (cx_u16)(18 | (r - 11) << 5);
                run -= r;
            }
            if (run >= 3) {
                out[k++] = (cx_u16)(17 | (run - 3) << 5);
                run = 0;
            }
        } else {
            out[k++] = v;
            run--;
            while (run >= 3) {
                int r = run < 6 ? run : 6;
                out[k++] = (cx_u16)(16 | (r - 3) << 5);
                run -= r;
            }
        }
        while (run-- > 0) out[k++] = v;
    }
    return k;
}

static void put_stored(deflater* D, cx_u32 from, cx_u32 to, int last) {
    do {
        cx_u32 len = to - from < 65535 ? to - from : 65535;
        int end = from + len == to;
        put(&D->w, (cx_u64)(last && end), 3);
        put_align(&D->w);
        cx_u8* p = D->w.p;
        p[0] = (cx_u8)len, p[1] = (cx_u8)(len >> 8), p[2] = (cx_u8)~len, p[3] = (cx_u8)(~len >> 8);
        memcpy(p + 4, D->in + from, len);
        D->w.p = p + 4 + len;
        from += len;
    } while (from < to);
}

static void put_symbols(deflater* D, const cx_u16* lcode, const cx_u8* llen, const cx_u16* dcode, const cx_u8* dlen) {
    bitout w = D->w;
    for (cx_u32 i = 0; i < D->nsyms; i++) {
        cx_u32 s = D->syms[i];
        if (s < 256) {
            put(&w, lcode[s], llen[s]);
            continue;
        }
        cx_u32 l3 = s & 255, dist = s >> 8 & 0xffff;
        cx_u32 lc = len_code[l3], ls = 257 + lc;
        put(&w, lcode[ls] | (cx_u64)(l3 + 3 - len_base[lc]) << llen[ls], llen[ls] + len_extra[lc]);
        cx_u32 dc = dist_code(dist);
        put(&w, dcode[dc] | (cx_u64)(dist - dist_base[dc]) << dlen[dc], dlen[dc] + dist_extra[dc]);
    }
    put(&w, lcode[256], llen[256]);
    D->w = w;
}

/* Writes the symbols so far as one block covering in[D->block..to), in whichever of the three
 * kinds is shortest, and starts the next. */
static void flush_block(deflater* D, cx_u32 to, int last) {
    D->lfreq[256] = 1;
    cx_u8 llen[286], dlen[30], cllen[19];
    cx_u16 lcode[286], dcode[30], clcode[19];
    code_lengths(D->lfreq, 286, 15, llen);
    code_lengths(D->dfreq, 30, 15, dlen);
    int nl = 286, nd = 30;
    while (nl > 257 && llen[nl - 1] == 0) nl--;
    while (nd > 1 && dlen[nd - 1] == 0) nd--;
    cx_u8 all[316];
    memcpy(all, llen, (size_t)nl);
    memcpy(all + nl, dlen, (size_t)nd);
    cx_u16 rle[316];
    int nrle = rle_lengths(all, nl + nd, rle);
    cx_u32 clfreq[19] = {0};
    for (int i = 0; i < nrle; i++) clfreq[rle[i] & 31]++;
    code_lengths(clfreq, 19, 7, cllen);
    int ncl = 19;
    while (ncl > 4 && cllen[cl_order[ncl - 1]] == 0) ncl--;

    /* The cost of each kind, in bits. */
    cx_u64 extra = 0, dyn = 3 + 14 + 3 * (cx_u64)ncl, fix = 3;
    for (int s = 0; s < 19; s++) dyn += (cx_u64)clfreq[s] * cllen[s];
    dyn += 2 * (cx_u64)clfreq[16] + 3 * (cx_u64)clfreq[17] + 7 * (cx_u64)clfreq[18];
    for (int s = 0; s < 286; s++) {
        dyn += (cx_u64)D->lfreq[s] * llen[s];
        fix += (cx_u64)D->lfreq[s] * fixed_llen[s];
        if (s >= 257) extra += (cx_u64)D->lfreq[s] * len_extra[s - 257];
    }
    for (int s = 0; s < 30; s++) {
        dyn += (cx_u64)D->dfreq[s] * dlen[s];
        fix += (cx_u64)D->dfreq[s] * 5;
        extra += (cx_u64)D->dfreq[s] * dist_extra[s];
    }
    dyn += extra, fix += extra;
    cx_u32 bytes = to - D->block;
    cx_u64 pieces = bytes / 65535 + (bytes % 65535 != 0 || bytes == 0);
    /* Padding to a byte is up to 7 bits; counting it all keeps stored from winning a tie it
     * might lose. */
    cx_u64 stored = (cx_u64)bytes * 8 + pieces * (3 + 7 + 32);

    if (stored <= dyn && stored <= fix) {
        put_stored(D, D->block, to, last);
    } else if (fix <= dyn) {
        put(&D->w, (cx_u64)(2 | last), 3);
        put_symbols(D, fixed_lcode, fixed_llen, fixed_dcode, fixed_dlen);
    } else {
        put(&D->w, (cx_u64)(4 | last), 3);
        make_codes(llen, 286, lcode);
        make_codes(dlen, 30, dcode);
        make_codes(cllen, 19, clcode);
        put(&D->w, (cx_u64)(nl - 257) | (cx_u64)(nd - 1) << 5 | (cx_u64)(ncl - 4) << 10, 14);
        for (int i = 0; i < ncl; i++) put(&D->w, cllen[cl_order[i]], 3);
        static const cx_u8 rep_bits[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};
        for (int i = 0; i < nrle; i++) {
            cx_u32 s = rle[i] & 31;
            put(&D->w, clcode[s] | (cx_u64)(rle[i] >> 5) << cllen[s], cllen[s] + rep_bits[s]);
        }
        put_symbols(D, lcode, llen, dcode, dlen);
    }
    D->nsyms = 0;
    D->block = to;
    memset(D->lfreq, 0, sizeof D->lfreq);
    memset(D->dfreq, 0, sizeof D->dfreq);
}

static inline void emit_literal(deflater* D, cx_u32 pos) {
    cx_u8 b = D->in[pos];
    D->syms[D->nsyms++] = b;
    D->lfreq[b]++;
    if (D->nsyms == BLOCK_SYMS) flush_block(D, pos + 1, 0);
}

static inline void emit_match(deflater* D, cx_u32 pos, cx_u32 len, cx_u32 dist) {
    D->syms[D->nsyms++] = 1u << 24 | dist << 8 | (len - 3);
    D->lfreq[257 + len_code[len - 3]]++;
    D->dfreq[dist_code(dist)]++;
    if (D->nsyms == BLOCK_SYMS) flush_block(D, pos + len, 0);
}

static void deflate_greedy(deflater* D, const level_cfg* c) {
    cx_u32 n = D->n, pos = 0;
    while (pos < n) {
        cx_u32 len = 0, dist = 0;
        if (pos + MIN_MATCH <= n) {
            cx_u32 h = hash4(D->in + pos, D->hbits), cand = D->head[h];
            D->prev[pos & D->wmask] = cand;
            D->head[h] = pos;
            if (cand < pos) len = longest(D, pos, cand, MIN_MATCH - 1, &dist, c->chain, c->nice);
        }
        if (len >= MIN_MATCH) {
            emit_match(D, pos, len, dist);
            cx_u32 stop = pos + len;
            if (len <= c->nice)
                for (cx_u32 p = pos + 1; p < stop && p + MIN_MATCH <= n; p++) insert(D, p);
            pos = stop;
        } else {
            emit_literal(D, pos);
            pos++;
        }
    }
}

/* At each position, the match there is taken only if the next position's isn't longer. */
static void deflate_lazy(deflater* D, const level_cfg* c) {
    cx_u32 n = D->n, pos = 0, prev_len = 0, prev_dist = 0;
    int pending = 0; /* in[pos - 1] is waiting to go out, as a literal or the start of prev_len */
    while (pos < n) {
        cx_u32 len = 0, dist = 0;
        if (pos + MIN_MATCH <= n) {
            cx_u32 h = hash4(D->in + pos, D->hbits), cand = D->head[h];
            D->prev[pos & D->wmask] = cand;
            D->head[h] = pos;
            if (cand < pos && prev_len < c->lazy) {
                cx_u32 chain = prev_len >= c->good ? c->chain >> 2 : c->chain;
                len = longest(D, pos, cand, prev_len > MIN_MATCH - 1 ? prev_len : MIN_MATCH - 1, &dist, chain ? chain : 1, c->nice);
                if (len <= prev_len) len = 0;
            }
        }
        if (pending && prev_len >= MIN_MATCH && len == 0) {
            emit_match(D, pos - 1, prev_len, prev_dist);
            cx_u32 stop = pos - 1 + prev_len;
            for (cx_u32 p = pos + 1; p < stop && p + MIN_MATCH <= n; p++) insert(D, p);
            pos = stop;
            pending = 0, prev_len = 0;
            continue;
        }
        if (pending) emit_literal(D, pos - 1);
        pending = 1;
        prev_len = len, prev_dist = dist;
        pos++;
    }
    if (pending) {
        if (prev_len >= MIN_MATCH) emit_match(D, pos - 1, prev_len, prev_dist);
        else emit_literal(D, pos - 1);
    }
}

/* At most how many bytes deflating `n` bytes takes: stored blocks, which a block falls back to
 * when nothing else is shorter, plus the wrapper, plus 8 for the last 8-byte store. */
cx_usize cx_deflate_bound(cx_usize n, cx_u32 format) {
    cx_usize wrap = format == F_ZLIB ? 6 : format == F_GZIP ? 18 : 0;
    return n + 6 * (n / 65535 + 1) + 6 * (n / BLOCK_SYMS + 2) + wrap + 16;
}

/* Deflates in[0..n), n below 2^32, at `level` 0 to 9, into `out`, which has room for
 * cx_deflate_bound(n, format) bytes. Returns how many it wrote. */
cx_usize cx_deflate(const cx_u8* in, cx_usize n, cx_u32 format, cx_u32 level, cx_u8* out) {
    once(&tables_ready, tables_init);
    cx_u8* o = out;
    if (format == F_ZLIB) {
        /* CINFO 7 for a 32 KiB window, and FLEVEL as zlib sets it, which Java's output has. */
        cx_u32 flevel = level < 2 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
        cx_u32 h = 0x78 << 8 | flevel << 6;
        h += (31 - h % 31) % 31;
        *o++ = (cx_u8)(h >> 8), *o++ = (cx_u8)h;
    } else if (format == F_GZIP) {
        static const cx_u8 head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
        memcpy(o, head, 10);
        o += 10;
    }
    if (level > 9) level = 9;
    deflater D;
    memset(&D, 0, sizeof D);
    D.in = in, D.n = (cx_u32)n, D.w.p = o;
    if (level == 0) {
        put_stored(&D, 0, (cx_u32)n, 1);
    } else {
        cx_u32 hbits = 8;
        while (hbits < 15 && (1u << hbits) < n) hbits++;
        cx_u32 wsize = 1u << hbits < WSIZE ? 1u << hbits : WSIZE;
        cx_u32 nsyms = n < BLOCK_SYMS ? (cx_u32)n + 1 : BLOCK_SYMS;
        cx_usize mem_size = ((cx_usize)(1u << hbits) + wsize + nsyms) * sizeof(cx_u32);
        void* mem = cx_alloc(mem_size);
        D.head = (cx_u32*)mem;
        D.prev = D.head + (1u << hbits);
        D.syms = D.prev + wsize;
        D.hbits = hbits, D.wmask = wsize - 1;
        memset(D.head, 0, (cx_usize)(1u << hbits) * sizeof(cx_u32));
        if (levels[level].lazy) deflate_lazy(&D, &levels[level]);
        else deflate_greedy(&D, &levels[level]);
        flush_block(&D, (cx_u32)n, 1);
        cx_free(mem, mem_size);
    }
    put_align(&D.w);
    o = D.w.p;
    if (format == F_ZLIB) {
        cx_u32 a = cx_zlib_adler32(1, in, n);
        *o++ = (cx_u8)(a >> 24), *o++ = (cx_u8)(a >> 16), *o++ = (cx_u8)(a >> 8), *o++ = (cx_u8)a;
    } else if (format == F_GZIP) {
        cx_u32 c = cx_zlib_crc32(0, in, n), s = (cx_u32)n;
        for (int i = 0; i < 4; i++) *o++ = (cx_u8)(c >> 8 * i);
        for (int i = 0; i < 4; i++) *o++ = (cx_u8)(s >> 8 * i);
    }
    return (cx_usize)(o - out);
}
