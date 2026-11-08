/* MD5, SHA-1, SHA-256, SHA-384 and SHA-512, and HMAC-SHA256, behind std.hash (D118). Linked only
 * into programs that call them. Written from RFC 1321, FIPS 180-4 and RFC 2104; the constants are
 * computed from their definitions: MD5's from sines, the SHA-2 ones from the cube and square roots
 * of the first primes. Vanilla hashes a name with MD5 for an offline player's UUID, the seed with
 * SHA-256 for the join packet, and the login's secret and key with SHA-1 for the session server;
 * scripts check what they download with SHA-256 and SHA-512.
 *
 * SHA-1 and SHA-256 use the processor's SHA extensions where it has them, which most x86-64
 * processors since 2017 do: four rounds an instruction, several times the speed of the plain
 * code, which is the fallback. MD5 is written out round by round, with each round's function in
 * the form that waits least on the round before. SHA-512 works in 64-bit words, eight rounds to a
 * loop so no variables are shuffled between them.
 *
 * A context is CX_DIGEST_CTX bytes the caller owns, a `List<u8>` in std.hash, so they're copied
 * in and out of a struct here and need no alignment. */
#include "calx_rt.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#define CX_X86 1
#endif

enum { MD5, SHA1, SHA256, SHA512, SHA384 };

typedef struct {
    union {
        cx_u32 w[16];
        cx_u64 q[8];
    } h;
    cx_u8 buf[128];
    cx_u64 len;    // bytes added
    cx_u32 kind;
    cx_u32 used;   // bytes waiting in `buf`
    cx_u32 ni;     // whether to use the SHA extensions
} digest;

_Static_assert(sizeof(digest) <= CX_DIGEST_CTX, "the context fits what std.hash makes room for");

static inline cx_u32 rotl(cx_u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static inline cx_u32 rotr(cx_u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static inline cx_u64 rotr64(cx_u64 x, int n) { return (x >> n) | (x << (64 - n)); }
static inline cx_u32 load_le(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}
static inline cx_u32 load_be(const cx_u8* p) { return __builtin_bswap32(load_le(p)); }
static inline cx_u64 load_be64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

/* Whether the processor has the SHA extensions, and SSSE3 and SSE4.1 beside them. */
static cx_u32 sha_ni(void) {
#ifdef CX_X86
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c >> 9 & 1) || !(c >> 19 & 1)) return 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) return b >> 29 & 1;
#endif
    return 0;
}

/* --- MD5 ----------------------------------------------------------------------------------- */

/* Each round adds the message word and constant first, which don't wait on the round before,
 * then the function of b, c and d, whose parts that don't need b are worked out first. The empty
 * asm keeps clang from moving the constant after the function, onto the chain each round waits
 * on, as it otherwise does. */
#define MD5_KEEP(v) __asm__("" : "+r"(v))
#define MD5_F(b, c, d) ((d) ^ ((b) & ((c) ^ (d))))
#define MD5_H(b, c, d) ((c) ^ (d) ^ (b))
#define MD5_I(b, c, d) ((c) ^ ((b) | ~(d)))
#define MD5_STEP(f, a, b, c, d, x, k, s) do { a += load_le(p + 4 * (x)) + (k); MD5_KEEP(a); a += f(b, c, d); a = rotl(a, s) + (b); } while (0)
/* G is (b & d) | (c & ~d), whose two halves share no bits, so they can be added in turn, and the
 * one without b goes first. */
#define MD5_STEP_G(a, b, c, d, x, k, s) do { a += load_le(p + 4 * (x)) + (k) + ((c) & ~(d)); MD5_KEEP(a); a += (b) & (d); a = rotl(a, s) + (b); } while (0)

static void md5_blocks(cx_u32* h, const cx_u8* p, cx_usize n) {
    cx_u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (; n; n--, p += 64) {
        cx_u32 aa = a, bb = b, cc = c, dd = d;
        MD5_STEP(MD5_F, a, b, c, d, 0, 0xd76aa478, 7);
        MD5_STEP(MD5_F, d, a, b, c, 1, 0xe8c7b756, 12);
        MD5_STEP(MD5_F, c, d, a, b, 2, 0x242070db, 17);
        MD5_STEP(MD5_F, b, c, d, a, 3, 0xc1bdceee, 22);
        MD5_STEP(MD5_F, a, b, c, d, 4, 0xf57c0faf, 7);
        MD5_STEP(MD5_F, d, a, b, c, 5, 0x4787c62a, 12);
        MD5_STEP(MD5_F, c, d, a, b, 6, 0xa8304613, 17);
        MD5_STEP(MD5_F, b, c, d, a, 7, 0xfd469501, 22);
        MD5_STEP(MD5_F, a, b, c, d, 8, 0x698098d8, 7);
        MD5_STEP(MD5_F, d, a, b, c, 9, 0x8b44f7af, 12);
        MD5_STEP(MD5_F, c, d, a, b, 10, 0xffff5bb1, 17);
        MD5_STEP(MD5_F, b, c, d, a, 11, 0x895cd7be, 22);
        MD5_STEP(MD5_F, a, b, c, d, 12, 0x6b901122, 7);
        MD5_STEP(MD5_F, d, a, b, c, 13, 0xfd987193, 12);
        MD5_STEP(MD5_F, c, d, a, b, 14, 0xa679438e, 17);
        MD5_STEP(MD5_F, b, c, d, a, 15, 0x49b40821, 22);
        MD5_STEP_G(a, b, c, d, 1, 0xf61e2562, 5);
        MD5_STEP_G(d, a, b, c, 6, 0xc040b340, 9);
        MD5_STEP_G(c, d, a, b, 11, 0x265e5a51, 14);
        MD5_STEP_G(b, c, d, a, 0, 0xe9b6c7aa, 20);
        MD5_STEP_G(a, b, c, d, 5, 0xd62f105d, 5);
        MD5_STEP_G(d, a, b, c, 10, 0x02441453, 9);
        MD5_STEP_G(c, d, a, b, 15, 0xd8a1e681, 14);
        MD5_STEP_G(b, c, d, a, 4, 0xe7d3fbc8, 20);
        MD5_STEP_G(a, b, c, d, 9, 0x21e1cde6, 5);
        MD5_STEP_G(d, a, b, c, 14, 0xc33707d6, 9);
        MD5_STEP_G(c, d, a, b, 3, 0xf4d50d87, 14);
        MD5_STEP_G(b, c, d, a, 8, 0x455a14ed, 20);
        MD5_STEP_G(a, b, c, d, 13, 0xa9e3e905, 5);
        MD5_STEP_G(d, a, b, c, 2, 0xfcefa3f8, 9);
        MD5_STEP_G(c, d, a, b, 7, 0x676f02d9, 14);
        MD5_STEP_G(b, c, d, a, 12, 0x8d2a4c8a, 20);
        MD5_STEP(MD5_H, a, b, c, d, 5, 0xfffa3942, 4);
        MD5_STEP(MD5_H, d, a, b, c, 8, 0x8771f681, 11);
        MD5_STEP(MD5_H, c, d, a, b, 11, 0x6d9d6122, 16);
        MD5_STEP(MD5_H, b, c, d, a, 14, 0xfde5380c, 23);
        MD5_STEP(MD5_H, a, b, c, d, 1, 0xa4beea44, 4);
        MD5_STEP(MD5_H, d, a, b, c, 4, 0x4bdecfa9, 11);
        MD5_STEP(MD5_H, c, d, a, b, 7, 0xf6bb4b60, 16);
        MD5_STEP(MD5_H, b, c, d, a, 10, 0xbebfbc70, 23);
        MD5_STEP(MD5_H, a, b, c, d, 13, 0x289b7ec6, 4);
        MD5_STEP(MD5_H, d, a, b, c, 0, 0xeaa127fa, 11);
        MD5_STEP(MD5_H, c, d, a, b, 3, 0xd4ef3085, 16);
        MD5_STEP(MD5_H, b, c, d, a, 6, 0x04881d05, 23);
        MD5_STEP(MD5_H, a, b, c, d, 9, 0xd9d4d039, 4);
        MD5_STEP(MD5_H, d, a, b, c, 12, 0xe6db99e5, 11);
        MD5_STEP(MD5_H, c, d, a, b, 15, 0x1fa27cf8, 16);
        MD5_STEP(MD5_H, b, c, d, a, 2, 0xc4ac5665, 23);
        MD5_STEP(MD5_I, a, b, c, d, 0, 0xf4292244, 6);
        MD5_STEP(MD5_I, d, a, b, c, 7, 0x432aff97, 10);
        MD5_STEP(MD5_I, c, d, a, b, 14, 0xab9423a7, 15);
        MD5_STEP(MD5_I, b, c, d, a, 5, 0xfc93a039, 21);
        MD5_STEP(MD5_I, a, b, c, d, 12, 0x655b59c3, 6);
        MD5_STEP(MD5_I, d, a, b, c, 3, 0x8f0ccc92, 10);
        MD5_STEP(MD5_I, c, d, a, b, 10, 0xffeff47d, 15);
        MD5_STEP(MD5_I, b, c, d, a, 1, 0x85845dd1, 21);
        MD5_STEP(MD5_I, a, b, c, d, 8, 0x6fa87e4f, 6);
        MD5_STEP(MD5_I, d, a, b, c, 15, 0xfe2ce6e0, 10);
        MD5_STEP(MD5_I, c, d, a, b, 6, 0xa3014314, 15);
        MD5_STEP(MD5_I, b, c, d, a, 13, 0x4e0811a1, 21);
        MD5_STEP(MD5_I, a, b, c, d, 4, 0xf7537e82, 6);
        MD5_STEP(MD5_I, d, a, b, c, 11, 0xbd3af235, 10);
        MD5_STEP(MD5_I, c, d, a, b, 2, 0x2ad7d2bb, 15);
        MD5_STEP(MD5_I, b, c, d, a, 9, 0xeb86d391, 21);
        a += aa, b += bb, c += cc, d += dd;
    }
    h[0] = a, h[1] = b, h[2] = c, h[3] = d;
}

/* --- SHA-1 --------------------------------------------------------------------------------- */

static void sha1_blocks(cx_u32* h, const cx_u8* p, cx_usize n) {
    for (; n; n--, p += 64) {
        cx_u32 w[80];
        for (int t = 0; t < 16; t++) w[t] = load_be(p + 4 * t);
        for (int t = 16; t < 80; t++) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        cx_u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; t++) {
            cx_u32 f, k;
            if (t < 20) f = (b & c) | (~b & d), k = 0x5a827999;
            else if (t < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
            else if (t < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
            else f = b ^ c ^ d, k = 0xca62c1d6;
            cx_u32 x = rotl(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = x;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
}

#ifdef CX_X86
#define SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))

/* Four rounds at a time. ABCD sits in one register with A in its top lane, and E in the top lane
 * of another, which sha1nexte works out for the next four rounds from A of the four before. The
 * message schedule is sha1msg1, an xor and sha1msg2 per four words. `f` picks the rounds'
 * function and constant, a fifth of the way at a time. */
#define SHA1_FOUR(g, f)                                                                          \
    do {                                                                                         \
        if (g < 4) {                                                                             \
            w[g & 3] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 16 * (g & 3))), flip); \
        } else {                                                                                 \
            w[g & 3] = _mm_sha1msg2_epu32(_mm_xor_si128(_mm_sha1msg1_epu32(w[g & 3], w[(g + 1) & 3]), w[(g + 2) & 3]), w[(g + 3) & 3]); \
        }                                                                                        \
        __m128i e1 = g == 0 ? _mm_add_epi32(e, w[0]) : _mm_sha1nexte_epu32(prev, w[g & 3]);      \
        prev = abcd;                                                                             \
        abcd = _mm_sha1rnds4_epu32(abcd, e1, f);                                                 \
    } while (0)

SHA_TARGET static void sha1_blocks_ni(cx_u32* h, const cx_u8* p, cx_usize n) {
    const __m128i flip = _mm_set_epi64x(0x0001020304050607LL, 0x08090a0b0c0d0e0fLL);
    __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i*)h), 0x1b);
    __m128i e = _mm_set_epi32((int)h[4], 0, 0, 0);
    for (; n; n--, p += 64) {
        __m128i abcd0 = abcd, e0 = e, prev = abcd, w[4];
        /* The first 16 words are the block's; each later four are made from the four before. */
        SHA1_FOUR(0, 0); SHA1_FOUR(1, 0); SHA1_FOUR(2, 0); SHA1_FOUR(3, 0); SHA1_FOUR(4, 0);
        SHA1_FOUR(5, 1); SHA1_FOUR(6, 1); SHA1_FOUR(7, 1); SHA1_FOUR(8, 1); SHA1_FOUR(9, 1);
        SHA1_FOUR(10, 2); SHA1_FOUR(11, 2); SHA1_FOUR(12, 2); SHA1_FOUR(13, 2); SHA1_FOUR(14, 2);
        SHA1_FOUR(15, 3); SHA1_FOUR(16, 3); SHA1_FOUR(17, 3); SHA1_FOUR(18, 3); SHA1_FOUR(19, 3);
        e = _mm_sha1nexte_epu32(prev, e0);
        abcd = _mm_add_epi32(abcd, abcd0);
    }
    _mm_storeu_si128((__m128i*)h, _mm_shuffle_epi32(abcd, 0x1b));
    h[4] = (cx_u32)_mm_extract_epi32(e, 3);
}
#endif

/* --- SHA-256 ------------------------------------------------------------------------------- */

static const cx_u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_blocks(cx_u32* h, const cx_u8* p, cx_usize n) {
    for (; n; n--, p += 64) {
        cx_u32 w[64];
        for (int t = 0; t < 16; t++) w[t] = load_be(p + 4 * t);
        for (int t = 16; t < 64; t++) {
            cx_u32 s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            cx_u32 s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        cx_u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; t++) {
            cx_u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[t] + w[t];
            cx_u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
}

#ifdef CX_X86
/* Four rounds at a time, as two sha256rnds2 of two rounds each. The state is in two registers,
 * ABEF and CDGH, as the instruction wants it. Words 16 on are four at a time: sha256msg1 adds
 * sigma0 of the next four, alignr brings the words seven back, and sha256msg2 adds sigma1. */
#define SHA256_FOUR(g)                                                                           \
    do {                                                                                         \
        if (g < 4) {                                                                             \
            w[g & 3] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 16 * (g & 3))), flip); \
        } else {                                                                                 \
            __m128i t = _mm_add_epi32(_mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]), _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4)); \
            w[g & 3] = _mm_sha256msg2_epu32(t, w[(g + 3) & 3]);                                  \
        }                                                                                        \
        __m128i m = _mm_add_epi32(w[g & 3], _mm_loadu_si128((const __m128i*)(sha256_k + 4 * g))); \
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, m);                                             \
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(m, 0x0e));                    \
    } while (0)

SHA_TARGET static void sha256_blocks_ni(cx_u32* h, const cx_u8* p, cx_usize n) {
    const __m128i flip = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i dcba = _mm_loadu_si128((const __m128i*)h);
    __m128i hgfe = _mm_loadu_si128((const __m128i*)(h + 4));
    __m128i cdab = _mm_shuffle_epi32(dcba, 0xb1);
    __m128i efgh = _mm_shuffle_epi32(hgfe, 0x1b);
    __m128i abef = _mm_alignr_epi8(cdab, efgh, 8);
    __m128i cdgh = _mm_blend_epi16(efgh, cdab, 0xf0);
    for (; n; n--, p += 64) {
        __m128i abef0 = abef, cdgh0 = cdgh, w[4];
        SHA256_FOUR(0); SHA256_FOUR(1); SHA256_FOUR(2); SHA256_FOUR(3);
        SHA256_FOUR(4); SHA256_FOUR(5); SHA256_FOUR(6); SHA256_FOUR(7);
        SHA256_FOUR(8); SHA256_FOUR(9); SHA256_FOUR(10); SHA256_FOUR(11);
        SHA256_FOUR(12); SHA256_FOUR(13); SHA256_FOUR(14); SHA256_FOUR(15);
        abef = _mm_add_epi32(abef, abef0);
        cdgh = _mm_add_epi32(cdgh, cdgh0);
    }
    __m128i feba = _mm_shuffle_epi32(abef, 0x1b);
    __m128i dchg = _mm_shuffle_epi32(cdgh, 0xb1);
    _mm_storeu_si128((__m128i*)h, _mm_blend_epi16(feba, dchg, 0xf0));
    _mm_storeu_si128((__m128i*)(h + 4), _mm_alignr_epi8(dchg, feba, 8));
}
#endif

/* --- SHA-512 and SHA-384 ------------------------------------------------------------------- */

static const cx_u64 sha512_k[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};
static const cx_u64 sha512_h[8] = {
    0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};
static const cx_u64 sha384_h[8] = {
    0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
    0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4,
};

/* `wk` holds each round's word with its constant already added. The sums go in the order their
 * parts are ready, the one waiting longest on the round before, Sigma1 of e, last, since that's
 * the chain each round waits on. Maj(a, b, c) is b ^ ((a ^ b) & (b ^ c)), and b ^ c is a ^ b of
 * the round before, kept in `bc`, which saves two operations a round. */
#define SHA512_ROUND(a, b, c, d, e, f, g, h, i)                                                  \
    do {                                                                                         \
        cx_u64 t1 = h + wk[i];                                                                   \
        t1 += g ^ (e & (f ^ g));                                                                 \
        t1 += rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);                                     \
        d += t1;                                                                                 \
        cx_u64 ab = a ^ b;                                                                       \
        h = t1 + (b ^ (ab & bc)) + (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39));              \
        bc = ab;                                                                                 \
    } while (0)

#ifdef CX_X86
static inline __m128i rotr_lanes(__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); }

/* Words 2j and 2j + 1, two at a time in SSE2's 64-bit lanes, which every x86-64 processor has, so
 * they take the vector units while the rounds take the scalar ones. The pair needs only words up
 * to 2j - 1, so it's one step: sigma1 of the pair before, the pair seven back, which straddles
 * two pairs, sigma0 of the pair fifteen back, and the pair sixteen back. The pair goes into `w`,
 * and with its constants added into `wk`. */
static inline void sha512_pair(__m128i* w, cx_u64* wk, int j) {
    __m128i x2 = w[j - 1];
    __m128i x7 = _mm_castpd_si128(_mm_shuffle_pd(_mm_castsi128_pd(w[j - 4]), _mm_castsi128_pd(w[j - 3]), 1));
    __m128i x15 = _mm_castpd_si128(_mm_shuffle_pd(_mm_castsi128_pd(w[j - 8]), _mm_castsi128_pd(w[j - 7]), 1));
    __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr_lanes(x15, 1), rotr_lanes(x15, 8)), _mm_srli_epi64(x15, 7));
    __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr_lanes(x2, 19), rotr_lanes(x2, 61)), _mm_srli_epi64(x2, 6));
    w[j] = _mm_add_epi64(_mm_add_epi64(w[j - 8], s0), _mm_add_epi64(x7, s1));
    _mm_storeu_si128((__m128i*)(wk + 2 * j), _mm_add_epi64(w[j], _mm_loadu_si128((const __m128i*)(sha512_k + 2 * j))));
}

/* Eight rounds a loop, each naming the variables as they've moved along, and with them the words
 * for the rounds sixteen on, so the schedule's work fills the gaps in the rounds' chain of
 * waits, as it couldn't if it all came first. */
static void sha512_blocks(cx_u64* hs, const cx_u8* p, cx_usize n) {
    for (; n; n--, p += 128) {
        __m128i w[40];
        cx_u64 wk[80];
        for (int j = 0; j < 8; j++) {
            w[j] = _mm_set_epi64x((long long)load_be64(p + 16 * j + 8), (long long)load_be64(p + 16 * j));
            _mm_storeu_si128((__m128i*)(wk + 2 * j), _mm_add_epi64(w[j], _mm_loadu_si128((const __m128i*)(sha512_k + 2 * j))));
        }
        cx_u64 a = hs[0], b = hs[1], c = hs[2], d = hs[3], e = hs[4], f = hs[5], g = hs[6], h = hs[7];
        cx_u64 bc = b ^ c;
        for (int i = 0; i < 80; i += 8) {
            if (i < 64) {
                sha512_pair(w, wk, i / 2 + 8);
                sha512_pair(w, wk, i / 2 + 9);
                sha512_pair(w, wk, i / 2 + 10);
                sha512_pair(w, wk, i / 2 + 11);
            }
            SHA512_ROUND(a, b, c, d, e, f, g, h, i);
            SHA512_ROUND(h, a, b, c, d, e, f, g, i + 1);
            SHA512_ROUND(g, h, a, b, c, d, e, f, i + 2);
            SHA512_ROUND(f, g, h, a, b, c, d, e, i + 3);
            SHA512_ROUND(e, f, g, h, a, b, c, d, i + 4);
            SHA512_ROUND(d, e, f, g, h, a, b, c, i + 5);
            SHA512_ROUND(c, d, e, f, g, h, a, b, i + 6);
            SHA512_ROUND(b, c, d, e, f, g, h, a, i + 7);
        }
        hs[0] += a, hs[1] += b, hs[2] += c, hs[3] += d, hs[4] += e, hs[5] += f, hs[6] += g, hs[7] += h;
    }
}
#else
static void sha512_blocks(cx_u64* hs, const cx_u8* p, cx_usize n) {
    for (; n; n--, p += 128) {
        cx_u64 w[80], wk[80];
        for (int t = 0; t < 16; t++) w[t] = load_be64(p + 8 * t);
        for (int t = 16; t < 80; t++) {
            cx_u64 s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
            cx_u64 s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        for (int t = 0; t < 80; t++) wk[t] = w[t] + sha512_k[t];
        cx_u64 a = hs[0], b = hs[1], c = hs[2], d = hs[3], e = hs[4], f = hs[5], g = hs[6], h = hs[7];
        cx_u64 bc = b ^ c;
        for (int i = 0; i < 80; i += 8) {
            SHA512_ROUND(a, b, c, d, e, f, g, h, i);
            SHA512_ROUND(h, a, b, c, d, e, f, g, i + 1);
            SHA512_ROUND(g, h, a, b, c, d, e, f, i + 2);
            SHA512_ROUND(f, g, h, a, b, c, d, e, i + 3);
            SHA512_ROUND(e, f, g, h, a, b, c, d, i + 4);
            SHA512_ROUND(d, e, f, g, h, a, b, c, i + 5);
            SHA512_ROUND(c, d, e, f, g, h, a, b, i + 6);
            SHA512_ROUND(b, c, d, e, f, g, h, a, i + 7);
        }
        hs[0] += a, hs[1] += b, hs[2] += c, hs[3] += d, hs[4] += e, hs[5] += f, hs[6] += g, hs[7] += h;
    }
}
#endif

/* --- every hash, through one context ------------------------------------------------------- */

static cx_usize block_size(const digest* d) { return d->kind >= SHA512 ? 128 : 64; }

static void blocks(digest* d, const cx_u8* p, cx_usize n) {
    switch (d->kind) {
        case MD5: md5_blocks(d->h.w, p, n); break;
#ifdef CX_X86
        case SHA1: if (d->ni) sha1_blocks_ni(d->h.w, p, n); else sha1_blocks(d->h.w, p, n); break;
        case SHA256: if (d->ni) sha256_blocks_ni(d->h.w, p, n); else sha256_blocks(d->h.w, p, n); break;
#else
        case SHA1: sha1_blocks(d->h.w, p, n); break;
        case SHA256: sha256_blocks(d->h.w, p, n); break;
#endif
        default: sha512_blocks(d->h.q, p, n); break;
    }
}

static void start(digest* d, cx_u32 kind) {
    static const cx_u32 md5_h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const cx_u32 sha1_h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    static const cx_u32 sha256_h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(d, 0, sizeof *d);
    d->kind = kind;
    d->ni = (kind == SHA1 || kind == SHA256) && sha_ni();
    switch (kind) {
        case MD5: memcpy(d->h.w, md5_h, sizeof md5_h); break;
        case SHA1: memcpy(d->h.w, sha1_h, sizeof sha1_h); break;
        case SHA256: memcpy(d->h.w, sha256_h, sizeof sha256_h); break;
        case SHA512: memcpy(d->h.q, sha512_h, sizeof sha512_h); break;
        default: memcpy(d->h.q, sha384_h, sizeof sha384_h); break;
    }
}

static void add(digest* d, const cx_u8* p, cx_usize n) {
    cx_usize size = block_size(d);
    d->len += n;
    if (d->used) {
        cx_usize take = size - d->used < n ? size - d->used : n;
        memcpy(d->buf + d->used, p, take);
        d->used += (cx_u32)take;
        p += take;
        n -= take;
        if (d->used < size) return;
        blocks(d, d->buf, 1);
        d->used = 0;
    }
    if (n >= size) blocks(d, p, n / size);
    p += n / size * size;
    n %= size;
    memcpy(d->buf, p, n);
    d->used = (cx_u32)n;
}

/* Pads with a 1 bit, zeros and the length in bits: 8 bytes of it, little-endian for MD5 and
 * big-endian for SHA-1 and SHA-256, or 16 big-endian for SHA-384 and SHA-512. Writes the digest
 * and returns its length. */
static cx_usize finish(digest* d, cx_u8* out) {
    cx_usize size = block_size(d), field = size / 8;
    cx_u8 pad[144] = {0x80};
    cx_usize zeros = (d->used < size - field ? size - field : 2 * size - field) - d->used;
    cx_u64 bits = d->len * 8;
    for (int i = 0; i < 8; i++) pad[zeros + field - 8 + i] = (cx_u8)(d->kind == MD5 ? bits >> (8 * i) : bits >> (56 - 8 * i));
    if (field == 16) {
        cx_u64 high = d->len >> 61;
        for (int i = 0; i < 8; i++) pad[zeros + i] = (cx_u8)(high >> (56 - 8 * i));
    }
    add(d, pad, zeros + field);
    if (d->kind >= SHA512) {
        cx_usize n = d->kind == SHA512 ? 64 : 48;
        for (cx_usize i = 0; i < n; i++) out[i] = (cx_u8)(d->h.q[i / 8] >> (56 - 8 * (i % 8)));
        return n;
    }
    cx_usize words = d->kind == MD5 ? 4 : d->kind == SHA1 ? 5 : 8;
    for (cx_usize i = 0; i < words; i++) {
        cx_u32 v = d->h.w[i];
        if (d->kind != MD5) v = __builtin_bswap32(v);
        memcpy(out + 4 * i, &v, 4);
    }
    return 4 * words;
}

void cx_digest_start(cx_u8* ctx, cx_u32 kind) {
    digest d;
    start(&d, kind);
    memcpy(ctx, &d, sizeof d);
}

void cx_digest_add(cx_u8* ctx, const cx_u8* p, cx_usize n) {
    digest d;
    memcpy(&d, ctx, sizeof d);
    add(&d, p, n);
    memcpy(ctx, &d, sizeof d);
}

cx_usize cx_digest_finish(cx_u8* ctx, cx_u8* out) {
    digest d;
    memcpy(&d, ctx, sizeof d);
    return finish(&d, out);
}

/* RFC 2104: SHA-256 of the key padded with 0x5c, then of the key padded with 0x36 and the message.
 * A key longer than a block is hashed first. */
void cx_digest_hmac_sha256(const cx_u8* key, cx_usize key_len, const cx_u8* msg, cx_usize msg_len, cx_u8* out) {
    cx_u8 k[64] = {0}, pad[64], inner[32];
    digest d;
    if (key_len > 64) {
        start(&d, SHA256);
        add(&d, key, key_len);
        finish(&d, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    start(&d, SHA256);
    add(&d, pad, 64);
    add(&d, msg, msg_len);
    finish(&d, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    start(&d, SHA256);
    add(&d, pad, 64);
    add(&d, inner, 32);
    finish(&d, out);
}
