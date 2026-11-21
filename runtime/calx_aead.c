/* Authenticated encryption behind std.crypto (CLX-397): ChaCha20-Poly1305 as RFC 8439 makes it,
 * XChaCha20-Poly1305 as draft-irtf-cfrg-xchacha makes it from HChaCha20, and AES-GCM with 128-,
 * 192- and 256-bit keys as NIST SP 800-38D makes it. Linked only into programs that call them.
 *
 * ChaCha20 runs four blocks at once in vectors of four 32-bit words, each vector one word of the
 * four blocks, which clang makes SSE2 or NEON. Poly1305 is three limbs of 44, 44 and 42 bits,
 * multiplied through 128-bit products. Neither has a branch or a table that depends on a secret.
 *
 * AES and GHASH use AES-NI and PCLMULQDQ where the processor has them, or Arm64's AES and PMULL,
 * eight blocks at a time, with GHASH's eight products summed before one reduction. Elsewhere AES
 * works out its S-box from GF(2^8) arithmetic on bit planes, the same bit of 64 bytes in each
 * word, and GHASH multiplies with integer multiplies whose bits are kept apart by holes, so
 * neither reads memory at an address that depends on the key or the data.
 *
 * Opening checks the tag, in a time that doesn't depend on where it differs, before anything is
 * decrypted, so a forged message gives back nothing. Keys and what's worked out from them are
 * wiped from the stack after each call. A GCM context is CX_GCM_CTX bytes the caller owns, a
 * `List<u8>` in std.crypto, copied in and out, so it needs no alignment. */
#include "calx_rt.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#define CX_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define CX_ARM 1
#endif

typedef unsigned __int128 u128;

static cx_u32 ld32(const cx_u8* p) { return (cx_u32)p[0] | (cx_u32)p[1] << 8 | (cx_u32)p[2] << 16 | (cx_u32)p[3] << 24; }
static void st32(cx_u8* p, cx_u32 v) {
    p[0] = (cx_u8)v;
    p[1] = (cx_u8)(v >> 8);
    p[2] = (cx_u8)(v >> 16);
    p[3] = (cx_u8)(v >> 24);
}
static cx_u64 ld64(const cx_u8* p) { return (cx_u64)ld32(p) | (cx_u64)ld32(p + 4) << 32; }
static void st64(cx_u8* p, cx_u64 v) {
    st32(p, (cx_u32)v);
    st32(p + 4, (cx_u32)(v >> 32));
}
static cx_u64 ld64be(const cx_u8* p) {
    cx_u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
static void st64be(cx_u8* p, cx_u64 v) {
    for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (cx_u8)v;
}

/* Zeroes what held a key or what was worked out from it. The empty asm, which might read the
 * memory, keeps clang from dropping the stores as dead. */
static inline void wipe(void* p, cx_usize n) {
    memset(p, 0, n);
    __asm__ volatile("" : : "r"(p) : "memory");
}

/* Whether the two 16-byte tags are the same, looking at every byte whatever the first differs. */
static int same_tag(const cx_u8* a, const cx_u8* b) {
    cx_u32 d = 0;
    for (int i = 0; i < 16; i++) d |= (cx_u32)(a[i] ^ b[i]);
    return (int)(1 & ((d - 1) >> 8));
}

/* --- ChaCha20 ------------------------------------------------------------------------------ */

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                                    \
    do {                                                  \
        a += b, d ^= a, d = ROTL(d, 16);                  \
        c += d, b ^= c, b = ROTL(b, 12);                  \
        a += b, d ^= a, d = ROTL(d, 8);                   \
        c += d, b ^= c, b = ROTL(b, 7);                   \
    } while (0)
#define DOUBLE_ROUND(x)                    \
    do {                                   \
        QR(x[0], x[4], x[8], x[12]);       \
        QR(x[1], x[5], x[9], x[13]);       \
        QR(x[2], x[6], x[10], x[14]);      \
        QR(x[3], x[7], x[11], x[15]);      \
        QR(x[0], x[5], x[10], x[15]);      \
        QR(x[1], x[6], x[11], x[12]);      \
        QR(x[2], x[7], x[8], x[13]);       \
        QR(x[3], x[4], x[9], x[14]);       \
    } while (0)

/* The state: "expand 32-byte k", the key, the block counter and the 12-byte nonce. */
static void chacha_setup(cx_u32 s[16], const cx_u8 key[32], cx_u32 counter, const cx_u8 nonce[12]) {
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32(nonce + 4 * i);
}

/* One block of key stream, 64 bytes. */
static void chacha_block(const cx_u32 s[16], cx_u8 out[64]) {
    cx_u32 x[16];
    memcpy(x, s, sizeof x);
    for (int r = 0; r < 10; r++) DOUBLE_ROUND(x);
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
    wipe(x, sizeof x);
}

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ChaCha20's vectors are stored as little-endian words"
#endif

typedef cx_u32 v4u32 __attribute__((vector_size(16)));
typedef cx_u32 v8u32 __attribute__((vector_size(32)));

/* Words w to w + 3 of four blocks, one vector each word, a lane each block, turned into four
 * vectors of one block each, xored with `in` and stored in `out`, whose blocks are 64 bytes
 * apart. */
static inline __attribute__((always_inline)) void chacha_put4(v4u32 a, v4u32 b, v4u32 c, v4u32 d, const cx_u8* in, cx_u8* out) {
    v4u32 t0 = __builtin_shufflevector(a, b, 0, 4, 1, 5), t1 = __builtin_shufflevector(a, b, 2, 6, 3, 7);
    v4u32 t2 = __builtin_shufflevector(c, d, 0, 4, 1, 5), t3 = __builtin_shufflevector(c, d, 2, 6, 3, 7);
    v4u32 r[4] = {__builtin_shufflevector(t0, t2, 0, 1, 4, 5), __builtin_shufflevector(t0, t2, 2, 3, 6, 7),
                  __builtin_shufflevector(t1, t3, 0, 1, 4, 5), __builtin_shufflevector(t1, t3, 2, 3, 6, 7)};
    for (int k = 0; k < 4; k++) {
        v4u32 m;
        memcpy(&m, in + 64 * k, 16);
        m ^= r[k];
        memcpy(out + 64 * k, &m, 16);
    }
}

/* Four blocks, for the counter and the three after it: 256 bytes of `in` xored into `out`. */
static void chacha_xor4(const cx_u32 s[16], const cx_u8* in, cx_u8* out) {
    v4u32 x[16], o[16];
    for (int i = 0; i < 16; i++) x[i] = (v4u32){s[i], s[i], s[i], s[i]};
    x[12] += (v4u32){0, 1, 2, 3};
    memcpy(o, x, sizeof o);
    for (int r = 0; r < 10; r++) DOUBLE_ROUND(x);
    for (int i = 0; i < 16; i++) x[i] += o[i];
    for (int w = 0; w < 16; w += 4) chacha_put4(x[w], x[w + 1], x[w + 2], x[w + 3], in + 4 * w, out + 4 * w);
    wipe(x, sizeof x);
    wipe(o, sizeof o);
}

#ifdef CX_X86
/* Eight blocks at once in AVX2's 256-bit vectors, 512 bytes, each vector's halves four blocks. */
__attribute__((target("avx2"))) static void chacha_xor8(const cx_u32 s[16], const cx_u8* in, cx_u8* out) {
    v8u32 x[16], o[16];
    for (int i = 0; i < 16; i++) x[i] = (v8u32){s[i], s[i], s[i], s[i], s[i], s[i], s[i], s[i]};
    x[12] += (v8u32){0, 1, 2, 3, 4, 5, 6, 7};
    memcpy(o, x, sizeof o);
    for (int r = 0; r < 10; r++) DOUBLE_ROUND(x);
    for (int i = 0; i < 16; i++) x[i] += o[i];
    for (int w = 0; w < 16; w += 4) {
        chacha_put4(__builtin_shufflevector(x[w], x[w], 0, 1, 2, 3), __builtin_shufflevector(x[w + 1], x[w + 1], 0, 1, 2, 3),
                    __builtin_shufflevector(x[w + 2], x[w + 2], 0, 1, 2, 3), __builtin_shufflevector(x[w + 3], x[w + 3], 0, 1, 2, 3),
                    in + 4 * w, out + 4 * w);
        chacha_put4(__builtin_shufflevector(x[w], x[w], 4, 5, 6, 7), __builtin_shufflevector(x[w + 1], x[w + 1], 4, 5, 6, 7),
                    __builtin_shufflevector(x[w + 2], x[w + 2], 4, 5, 6, 7), __builtin_shufflevector(x[w + 3], x[w + 3], 4, 5, 6, 7),
                    in + 256 + 4 * w, out + 256 + 4 * w);
    }
    wipe(x, sizeof x);
    wipe(o, sizeof o);
}

/* Whether the processor and the system have AVX2, asked once, as calx_digest.c asks. */
static int chacha_avx2(void) {
    static int known = -1;
    int k = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (k >= 0) return k;
    k = 0;
    unsigned a, b, c, d, lo, hi;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c >> 27 & 1) && (c >> 28 & 1)) {
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        (void)hi;
        if ((lo & 6) == 6 && __get_cpuid_count(7, 0, &a, &b, &c, &d)) k = b >> 5 & 1;
    }
    __atomic_store_n(&known, k, __ATOMIC_RELAXED);
    return k;
}
#endif

/* `out` is `in` xored with the key stream from block `counter` on. */
static void chacha_xor(const cx_u8 key[32], cx_u32 counter, const cx_u8 nonce[12], const cx_u8* in, cx_u8* out, cx_usize n) {
    cx_u32 s[16];
    chacha_setup(s, key, counter, nonce);
#ifdef CX_X86
    if (n >= 512 && chacha_avx2()) {
        for (; n >= 512; in += 512, out += 512, n -= 512, s[12] += 8) chacha_xor8(s, in, out);
    }
#endif
    for (; n >= 256; in += 256, out += 256, n -= 256, s[12] += 4) chacha_xor4(s, in, out);
    if (n) {
        cx_u8 ks[256] = {0};
        chacha_xor4(s, ks, ks);
        for (cx_usize i = 0; i < n; i++) out[i] = (cx_u8)(in[i] ^ ks[i]);
        wipe(ks, sizeof ks);
    }
    wipe(s, sizeof s);
}

/* HChaCha20: the rounds over the key and the nonce's first 16 bytes, words 0 to 3 and 12 to 15 of
 * the result with nothing added back, a key of its own for each nonce. */
static void hchacha(const cx_u8 key[32], const cx_u8 nonce[16], cx_u8 out[32]) {
    cx_u32 x[16];
    chacha_setup(x, key, ld32(nonce), nonce + 4);
    for (int r = 0; r < 10; r++) DOUBLE_ROUND(x);
    for (int i = 0; i < 4; i++) {
        st32(out + 4 * i, x[i]);
        st32(out + 16 + 4 * i, x[12 + i]);
    }
    wipe(x, sizeof x);
}

/* --- Poly1305 ------------------------------------------------------------------------------ */

#define M44 0xfffffffffffull
#define M42 0x3ffffffffffull

typedef struct {
    cx_u64 r[3], h[3], pad[2];
} poly;

static void poly_init(poly* st, const cx_u8 key[32]) {
    cx_u64 t0 = ld64(key), t1 = ld64(key + 8);
    st->r[0] = t0 & 0xffc0fffffffull;
    st->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffffull;
    st->r[2] = (t1 >> 24) & 0x00ffffffc0full;
    st->h[0] = st->h[1] = st->h[2] = 0;
    st->pad[0] = ld64(key + 16);
    st->pad[1] = ld64(key + 24);
}

/* Whole 16-byte blocks, each with the 1 above its top byte. */
static void poly_blocks(poly* st, const cx_u8* m, cx_usize n) {
    const cx_u64 r0 = st->r[0], r1 = st->r[1], r2 = st->r[2];
    const cx_u64 s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
    cx_u64 h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    for (; n >= 16; m += 16, n -= 16) {
        cx_u64 t0 = ld64(m), t1 = ld64(m + 8);
        h0 += t0 & M44;
        h1 += ((t0 >> 44) | (t1 << 20)) & M44;
        h2 += ((t1 >> 24) & M42) | ((cx_u64)1 << 40);
        u128 d0 = (u128)h0 * r0 + (u128)h1 * s2 + (u128)h2 * s1;
        u128 d1 = (u128)h0 * r1 + (u128)h1 * r0 + (u128)h2 * s2;
        u128 d2 = (u128)h0 * r2 + (u128)h1 * r1 + (u128)h2 * r0;
        cx_u64 c = (cx_u64)(d0 >> 44);
        h0 = (cx_u64)d0 & M44;
        d1 += c;
        c = (cx_u64)(d1 >> 44);
        h1 = (cx_u64)d1 & M44;
        d2 += c;
        c = (cx_u64)(d2 >> 42);
        h2 = (cx_u64)d2 & M42;
        h0 += c * 5;
        c = h0 >> 44;
        h0 &= M44;
        h1 += c;
    }
    st->h[0] = h0;
    st->h[1] = h1;
    st->h[2] = h2;
}

/* The bytes, and then zeros up to a multiple of 16, as the AEAD pads its parts. */
static void poly_padded(poly* st, const cx_u8* m, cx_usize n) {
    cx_usize whole = n & ~(cx_usize)15;
    poly_blocks(st, m, whole);
    if (n > whole) {
        cx_u8 last[16] = {0};
        memcpy(last, m + whole, n - whole);
        poly_blocks(st, last, 16);
        wipe(last, sizeof last);
    }
}

/* h mod 2^130 - 5, plus the pad, mod 2^128. The subtraction of p is kept or not by a mask. */
static void poly_finish(poly* st, cx_u8 mac[16]) {
    cx_u64 h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], c;
    c = h1 >> 44, h1 &= M44;
    h2 += c, c = h2 >> 42, h2 &= M42;
    h0 += c * 5, c = h0 >> 44, h0 &= M44;
    h1 += c, c = h1 >> 44, h1 &= M44;
    h2 += c, c = h2 >> 42, h2 &= M42;
    h0 += c * 5, c = h0 >> 44, h0 &= M44;
    h1 += c;
    cx_u64 g0 = h0 + 5;
    c = g0 >> 44, g0 &= M44;
    cx_u64 g1 = h1 + c;
    c = g1 >> 44, g1 &= M44;
    cx_u64 g2 = h2 + c - ((cx_u64)1 << 42);
    c = (g2 >> 63) - 1;  // all ones when h >= p, so h - p is taken
    h0 = (h0 & ~c) | (g0 & c);
    h1 = (h1 & ~c) | (g1 & c);
    h2 = (h2 & ~c) | (g2 & c);
    cx_u64 t0 = st->pad[0], t1 = st->pad[1];
    h0 += t0 & M44, c = h0 >> 44, h0 &= M44;
    h1 += (((t0 >> 44) | (t1 << 20)) & M44) + c, c = h1 >> 44, h1 &= M44;
    h2 += ((t1 >> 24) & M42) + c, h2 &= M42;
    st64(mac, h0 | (h1 << 44));
    st64(mac + 8, (h1 >> 20) | (h2 << 24));
}

/* --- ChaCha20-Poly1305 --------------------------------------------------------------------- */

/* The tag: Poly1305 keyed by block 0's first 32 bytes, over the associated data and the
 * ciphertext, each padded to 16 bytes, then their lengths as two 64-bit little-endian numbers. */
static void chacha_tag(const cx_u8 key[32], const cx_u8 nonce[12], const cx_u8* aad, cx_usize alen, const cx_u8* ct, cx_usize clen, cx_u8 tag[16]) {
    cx_u32 s[16];
    cx_u8 block[64], lens[16];
    poly st;
    chacha_setup(s, key, 0, nonce);
    chacha_block(s, block);
    poly_init(&st, block);
    poly_padded(&st, aad, alen);
    poly_padded(&st, ct, clen);
    st64(lens, (cx_u64)alen);
    st64(lens + 8, (cx_u64)clen);
    poly_blocks(&st, lens, 16);
    poly_finish(&st, tag);
    wipe(s, sizeof s);
    wipe(block, sizeof block);
    wipe(&st, sizeof st);
}

/* The key and 12-byte nonce ChaCha20 runs with: the ones given for a 12-byte nonce, or for a
 * 24-byte one, XChaCha20's, HChaCha20 of the key and the nonce's first 16 bytes, and 4 zero bytes
 * then the nonce's last 8. */
static void chacha_inputs(const cx_u8* key, const cx_u8* nonce, cx_usize nonce_len, cx_u8 k[32], cx_u8 n[12]) {
    if (nonce_len == 24) {
        hchacha(key, nonce, k);
        memset(n, 0, 4);
        memcpy(n + 4, nonce + 16, 8);
    } else {
        memcpy(k, key, 32);
        memcpy(n, nonce, 12);
    }
}

void cx_aead_chacha_seal(const cx_u8* key, const cx_u8* nonce, cx_usize nonce_len, const cx_u8* pt, cx_usize n, const cx_u8* aad, cx_usize alen, cx_u8* out) {
    cx_u8 k[32], nn[12];
    chacha_inputs(key, nonce, nonce_len, k, nn);
    chacha_xor(k, 1, nn, pt, out, n);
    chacha_tag(k, nn, aad, alen, out, n, out + n);
    wipe(k, sizeof k);
}

cx_bool cx_aead_chacha_open(const cx_u8* key, const cx_u8* nonce, cx_usize nonce_len, const cx_u8* sealed, cx_usize n, const cx_u8* aad, cx_usize alen, cx_u8* out) {
    if (n < 16) return 0;
    n -= 16;
    cx_u8 k[32], nn[12], tag[16];
    chacha_inputs(key, nonce, nonce_len, k, nn);
    chacha_tag(k, nn, aad, alen, sealed, n, tag);
    int ok = same_tag(tag, sealed + n);
    if (ok) chacha_xor(k, 1, nn, sealed, out, n);
    wipe(k, sizeof k);
    return ok != 0;
}

/* --- AES without its instructions ---------------------------------------------------------- */

static cx_u8 xtime(cx_u8 x) { return (cx_u8)((x << 1) ^ ((x >> 7) * 0x1b)); }

/* r = a b in GF(2^8), mod x^8 + x^4 + x^3 + x + 1, on bit planes: bit k of a[i] is bit i of the
 * k-th byte. r may be a or b. */
static void gf_mul_planes(cx_u64 r[8], const cx_u64 a[8], const cx_u64 b[8]) {
    cx_u64 p[15] = {0};
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) p[i + j] ^= a[i] & b[j];
    }
    for (int k = 14; k >= 8; k--) {
        p[k - 4] ^= p[k];
        p[k - 5] ^= p[k];
        p[k - 7] ^= p[k];
        p[k - 8] ^= p[k];
    }
    memcpy(r, p, 8 * sizeof(cx_u64));
}

/* r = a^2, which is linear over GF(2): bit i goes to 2i, then the same reduction. */
static void gf_sq_planes(cx_u64 r[8], const cx_u64 a[8]) {
    cx_u64 p[15] = {0};
    for (int i = 0; i < 8; i++) p[2 * i] = a[i];
    for (int k = 14; k >= 8; k--) {
        p[k - 4] ^= p[k];
        p[k - 5] ^= p[k];
        p[k - 7] ^= p[k];
        p[k - 8] ^= p[k];
    }
    memcpy(r, p, 8 * sizeof(cx_u64));
}

/* Eight bytes as an 8x8 matrix of bits, byte j's bit i at 8j + i, turned so it's at 8i + j. */
static cx_u64 transpose8(cx_u64 x) {
    cx_u64 t;
    t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAull, x ^= t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCull, x ^= t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ull, x ^= t ^ (t << 28);
    return x;
}

/* The S-box of up to 64 bytes: each byte's inverse, as x^254, which takes 0 to 0, by 4 multiplies
 * and 7 squarings, then the affine map. The bytes go into bit planes eight at a time: byte i of
 * group g's turned matrix is bit i of its eight bytes, which is byte g of plane i. */
static void sub_bytes_ct(cx_u8* bytes, int n) {
    cx_u64 x[8] = {0}, x3[8], x6[8], x7[8], t[8];
    cx_u8 pad[64] = {0};
    memcpy(pad, bytes, (cx_usize)n);
    int groups = (n + 7) / 8;
    for (int g = 0; g < groups; g++) {
        cx_u64 m = transpose8(ld64(pad + 8 * g));
        for (int i = 0; i < 8; i++) x[i] |= ((m >> (8 * i)) & 0xFF) << (8 * g);
    }
    gf_sq_planes(t, x);        // x^2
    gf_mul_planes(x3, t, x);   // x^3
    gf_sq_planes(x6, x3);      // x^6
    gf_mul_planes(x7, x6, x);  // x^7
    gf_sq_planes(t, x6);       // x^12
    gf_mul_planes(t, t, x3);   // x^15
    gf_sq_planes(t, t);        // x^30
    gf_sq_planes(t, t);        // x^60
    gf_sq_planes(t, t);        // x^120
    gf_mul_planes(t, t, x7);   // x^127
    gf_sq_planes(t, t);        // x^254
    for (int i = 0; i < 8; i++) {
        x[i] = t[i] ^ t[(i + 4) & 7] ^ t[(i + 5) & 7] ^ t[(i + 6) & 7] ^ t[(i + 7) & 7] ^ ((0x63 >> i) & 1 ? ~(cx_u64)0 : 0);
    }
    for (int g = 0; g < groups; g++) {
        cx_u64 m = 0;
        for (int i = 0; i < 8; i++) m |= ((x[i] >> (8 * g)) & 0xFF) << (8 * i);
        st64(pad + 8 * g, transpose8(m));
    }
    memcpy(bytes, pad, (cx_usize)n);
    wipe(pad, sizeof pad);
    wipe(x, sizeof x);
    wipe(x3, sizeof x3);
    wipe(x6, sizeof x6);
    wipe(x7, sizeof x7);
    wipe(t, sizeof t);
}

/* FIPS 197's key expansion, for a key of nk words, 4, 6 or 8: 11, 13 or 15 round keys. */
static void aes_expand(const cx_u8* key, int nk, cx_u8 rk[15][16]) {
    int words = 4 * (nk + 7);
    cx_u8 w[60][4];
    memcpy(w, key, (cx_usize)(4 * nk));
    cx_u8 rcon = 1;
    for (int i = nk; i < words; i++) {
        cx_u8 t[4];
        memcpy(t, w[i - 1], 4);
        if (i % nk == 0) {
            cx_u8 first = t[0];
            t[0] = t[1], t[1] = t[2], t[2] = t[3], t[3] = first;
            sub_bytes_ct(t, 4);
            t[0] ^= rcon;
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            sub_bytes_ct(t, 4);
        }
        for (int j = 0; j < 4; j++) w[i][j] = (cx_u8)(w[i - nk][j] ^ t[j]);
        wipe(t, sizeof t);
    }
    memcpy(rk, w, (cx_usize)(4 * words));
    wipe(w, sizeof w);
}

/* Up to four blocks encrypted where they are, every round's S-boxes for all of them at once. */
static void aes_blocks_ct(const cx_u8 rk[15][16], int rounds, cx_u8* s, int blocks) {
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 16; i++) s[16 * b + i] ^= rk[0][i];
    }
    for (int r = 1; r <= rounds; r++) {
        sub_bytes_ct(s, 16 * blocks);
        for (int b = 0; b < blocks; b++) {
            cx_u8* w = s + 16 * b;
            cx_u8 t[16];
            for (int i = 0; i < 16; i++) t[i] = w[(i + 4 * (i & 3)) & 15];  // ShiftRows
            if (r < rounds) {
                for (int c = 0; c < 4; c++) {
                    cx_u8* m = t + 4 * c;
                    cx_u8 a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3], all = a0 ^ a1 ^ a2 ^ a3;
                    m[0] ^= all ^ xtime(a0 ^ a1);
                    m[1] ^= all ^ xtime(a1 ^ a2);
                    m[2] ^= all ^ xtime(a2 ^ a3);
                    m[3] ^= all ^ xtime(a3 ^ a0);
                }
            }
            for (int i = 0; i < 16; i++) w[i] = t[i] ^ rk[r][i];
            wipe(t, sizeof t);
        }
    }
}

/* --- GHASH without carry-less multiplies --------------------------------------------------- */

/* The low 64 bits of x times y without carries, from four integer multiplies of every fourth
 * bit, whose carries land in the holes between and are masked away (BearSSL's ghash_ctmul64). */
static cx_u64 bmul64(cx_u64 x, cx_u64 y) {
    const cx_u64 m1 = 0x1111111111111111ull, m2 = m1 << 1, m4 = m1 << 2, m8 = m1 << 3;
    cx_u64 x0 = x & m1, x1 = x & m2, x2 = x & m4, x3 = x & m8;
    cx_u64 y0 = y & m1, y1 = y & m2, y2 = y & m4, y3 = y & m8;
    cx_u64 z0 = (x0 * y0) ^ (x1 * y3) ^ (x2 * y2) ^ (x3 * y1);
    cx_u64 z1 = (x0 * y1) ^ (x1 * y0) ^ (x2 * y3) ^ (x3 * y2);
    cx_u64 z2 = (x0 * y2) ^ (x1 * y1) ^ (x2 * y0) ^ (x3 * y3);
    cx_u64 z3 = (x0 * y3) ^ (x1 * y2) ^ (x2 * y1) ^ (x3 * y0);
    return (z0 & m1) | (z1 & m2) | (z2 & m4) | (z3 & m8);
}

static cx_u64 rev64(cx_u64 x) {
    x = ((x & 0x5555555555555555ull) << 1) | ((x >> 1) & 0x5555555555555555ull);
    x = ((x & 0x3333333333333333ull) << 2) | ((x >> 2) & 0x3333333333333333ull);
    x = ((x & 0x0F0F0F0F0F0F0F0Full) << 4) | ((x >> 4) & 0x0F0F0F0F0F0F0F0Full);
    x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
    x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFull);
    return (x << 32) | (x >> 32);
}

/* y = (y xor each block) H, over the bytes, the last block padded with zeros. The high halves of
 * the 64-bit products come from the bit-reversed halves' low ones (Karatsuba for the middle). */
static void ghash_ct(const cx_u8 h[16], cx_u8 y[16], const cx_u8* p, cx_usize n) {
    cx_u64 y1 = ld64be(y), y0 = ld64be(y + 8);
    cx_u64 h1 = ld64be(h), h0 = ld64be(h + 8);
    cx_u64 h0r = rev64(h0), h1r = rev64(h1), h2 = h0 ^ h1, h2r = h0r ^ h1r;
    while (n) {
        cx_u8 last[16];
        const cx_u8* src = p;
        if (n >= 16) {
            p += 16;
            n -= 16;
        } else {
            memset(last, 0, 16);
            memcpy(last, p, n);
            src = last;
            n = 0;
        }
        y1 ^= ld64be(src);
        y0 ^= ld64be(src + 8);
        cx_u64 y0r = rev64(y0), y1r = rev64(y1), y2 = y0 ^ y1, y2r = y0r ^ y1r;
        cx_u64 z0 = bmul64(y0, h0), z1 = bmul64(y1, h1), z2 = bmul64(y2, h2);
        cx_u64 z0h = bmul64(y0r, h0r), z1h = bmul64(y1r, h1r), z2h = bmul64(y2r, h2r);
        z2 ^= z0 ^ z1;
        z2h ^= z0h ^ z1h;
        z0h = rev64(z0h) >> 1;
        z1h = rev64(z1h) >> 1;
        z2h = rev64(z2h) >> 1;
        cx_u64 v0 = z0, v1 = z0h ^ z2, v2 = z1 ^ z2h, v3 = z1h;
        v3 = (v3 << 1) | (v2 >> 63);
        v2 = (v2 << 1) | (v1 >> 63);
        v1 = (v1 << 1) | (v0 >> 63);
        v0 = v0 << 1;
        v2 ^= v0 ^ (v0 >> 1) ^ (v0 >> 2) ^ (v0 >> 7);
        v1 ^= (v0 << 63) ^ (v0 << 62) ^ (v0 << 57);
        v3 ^= v1 ^ (v1 >> 1) ^ (v1 >> 2) ^ (v1 >> 7);
        v2 ^= (v1 << 63) ^ (v1 << 62) ^ (v1 << 57);
        y0 = v2;
        y1 = v3;
    }
    st64be(y, y1);
    st64be(y + 8, y0);
}

/* --- AES-GCM ------------------------------------------------------------------------------- */

typedef struct {
    cx_u8 rk[15][16];   // the round keys
    cx_u8 h[16];        // H, the zero block encrypted
    cx_u8 hpow[8][16];  // H to H^8 for the instructions, each byte-reversed
    cx_u32 rounds;      // 10, 12 or 14
    cx_u32 ni;          // whether to use the AES and carry-less multiply instructions
} gcm;

_Static_assert(sizeof(gcm) <= CX_GCM_CTX, "the context fits what std.crypto makes room for");

/* Whether the processor has AES-NI, PCLMULQDQ, SSSE3 and SSE4.1, or Arm64's AES and PMULL, asked
 * once. Threads that race to ask store the same answer. */
static cx_u32 gcm_ni(void) {
    static int known = -1;
    int k = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (k >= 0) return (cx_u32)k;
    k = 0;
#ifdef CX_X86
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) k = (c >> 25 & 1) && (c >> 1 & 1) && (c >> 9 & 1) && (c >> 19 & 1);
#elif defined(CX_ARM)
    k = (cx_arm_features() & CX_ARM_AES) != 0;  // which the runtime sets only with PMULL too
#endif
    __atomic_store_n(&known, k, __ATOMIC_RELAXED);
    return (cx_u32)k;
}

#if defined(CX_X86) || defined(CX_ARM)
/* The same GHASH and CTR code for both, over a few operations each defines on 128-bit vectors. */
#ifdef CX_X86
#define NI __attribute__((target("aes,pclmul,ssse3,sse4.1"), always_inline)) static inline
#define NI_FN __attribute__((target("aes,pclmul,ssse3,sse4.1"))) static
typedef __m128i vec;
NI vec v_load(const cx_u8* p) { return _mm_loadu_si128((const __m128i*)p); }
NI void v_store(cx_u8* p, vec v) { _mm_storeu_si128((__m128i*)p, v); }
NI vec v_xor(vec a, vec b) { return _mm_xor_si128(a, b); }
NI vec v_or(vec a, vec b) { return _mm_or_si128(a, b); }
NI vec v_zero(void) { return _mm_setzero_si128(); }
NI vec v_rev(vec v) { return _mm_shuffle_epi8(v, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)); }
#define V_BYTES_UP(v, n) _mm_slli_si128(v, n)    // byte i takes byte i - n
#define V_BYTES_DOWN(v, n) _mm_srli_si128(v, n)  // byte i takes byte i + n
#define V_LANES_UP(v, n) _mm_slli_epi32(v, n)    // each 32-bit lane shifted up
#define V_LANES_DOWN(v, n) _mm_srli_epi32(v, n)
/* The four 64-bit products of a and b: the low halves', the high halves', and the two crossed. */
NI void v_clmul(vec a, vec b, vec* lo, vec* mid, vec* hi) {
    *lo = v_xor(*lo, _mm_clmulepi64_si128(a, b, 0x00));
    *hi = v_xor(*hi, _mm_clmulepi64_si128(a, b, 0x11));
    *mid = v_xor(*mid, v_xor(_mm_clmulepi64_si128(a, b, 0x10), _mm_clmulepi64_si128(a, b, 0x01)));
}
NI vec v_counter(vec base, cx_u32 ctr) { return _mm_insert_epi32(base, (int)__builtin_bswap32(ctr), 3); }
NI void aes8(vec x[8], const vec* k, int rounds, int count) {
    for (int j = 0; j < count; j++) x[j] = v_xor(x[j], k[0]);
    for (int r = 1; r < rounds; r++) {
        for (int j = 0; j < count; j++) x[j] = _mm_aesenc_si128(x[j], k[r]);
    }
    for (int j = 0; j < count; j++) x[j] = _mm_aesenclast_si128(x[j], k[rounds]);
}
#else
#define NI __attribute__((target("aes"), always_inline)) static inline
#define NI_FN __attribute__((target("aes"))) static
typedef uint8x16_t vec;
NI vec v_load(const cx_u8* p) { return vld1q_u8(p); }
NI void v_store(cx_u8* p, vec v) { vst1q_u8(p, v); }
NI vec v_xor(vec a, vec b) { return veorq_u8(a, b); }
NI vec v_or(vec a, vec b) { return vorrq_u8(a, b); }
NI vec v_zero(void) { return vdupq_n_u8(0); }
NI vec v_rev(vec v) {
    vec r = vrev64q_u8(v);
    return vextq_u8(r, r, 8);
}
#define V_BYTES_UP(v, n) vextq_u8(vdupq_n_u8(0), v, 16 - (n))
#define V_BYTES_DOWN(v, n) vextq_u8(v, vdupq_n_u8(0), n)
#define V_LANES_UP(v, n) vreinterpretq_u8_u32(vshlq_n_u32(vreinterpretq_u32_u8(v), n))
#define V_LANES_DOWN(v, n) vreinterpretq_u8_u32(vshrq_n_u32(vreinterpretq_u32_u8(v), n))
NI void v_clmul(vec a, vec b, vec* lo, vec* mid, vec* hi) {
    poly64x2_t pa = vreinterpretq_p64_u8(a), pb = vreinterpretq_p64_u8(b);
    poly64x2_t pc = vreinterpretq_p64_u8(vextq_u8(b, b, 8));  // b's halves swapped
    *lo = v_xor(*lo, vreinterpretq_u8_p128(vmull_p64(vgetq_lane_p64(pa, 0), vgetq_lane_p64(pb, 0))));
    *hi = v_xor(*hi, vreinterpretq_u8_p128(vmull_high_p64(pa, pb)));
    *mid = v_xor(*mid, v_xor(vreinterpretq_u8_p128(vmull_p64(vgetq_lane_p64(pa, 0), vgetq_lane_p64(pc, 0))),
                             vreinterpretq_u8_p128(vmull_high_p64(pa, pc))));
}
NI vec v_counter(vec base, cx_u32 ctr) {
    return vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(ctr), vreinterpretq_u32_u8(base), 3));
}
/* aese is AddRoundKey, ShiftRows and SubBytes, and aesmc MixColumns, so a round key goes in
 * before each round, and the last is xored on after the last, as calx_crypto.c's CFB8 does. */
NI void aes8(vec x[8], const vec* k, int rounds, int count) {
    for (int r = 0; r < rounds - 1; r++) {
        for (int j = 0; j < count; j++) x[j] = vaesmcq_u8(vaeseq_u8(x[j], k[r]));
    }
    for (int j = 0; j < count; j++) x[j] = v_xor(vaeseq_u8(x[j], k[rounds - 1]), k[rounds]);
}
#endif

/* The 256-bit product lo + mid 2^64 + hi 2^128 of byte-reversed blocks, reduced mod GHASH's
 * polynomial, as Intel's white paper on carry-less multiplication for GCM does it: shifted up a
 * bit, since the bits are reflected, then folded down by x^128 = x^7 + x^2 + x + 1. Products of
 * several pairs can be added before it, since it's linear. */
NI vec v_reduce(vec lo, vec mid, vec hi) {
    lo = v_xor(lo, V_BYTES_UP(mid, 8));
    hi = v_xor(hi, V_BYTES_DOWN(mid, 8));
    vec c_lo = V_LANES_DOWN(lo, 31), c_hi = V_LANES_DOWN(hi, 31);
    lo = V_LANES_UP(lo, 1);
    hi = V_LANES_UP(hi, 1);
    vec top = V_BYTES_DOWN(c_lo, 12);
    c_hi = V_BYTES_UP(c_hi, 4);
    c_lo = V_BYTES_UP(c_lo, 4);
    lo = v_or(lo, c_lo);
    hi = v_or(v_or(hi, c_hi), top);
    vec a = v_xor(v_xor(V_LANES_UP(lo, 31), V_LANES_UP(lo, 30)), V_LANES_UP(lo, 25));
    vec carry = V_BYTES_DOWN(a, 4);
    lo = v_xor(lo, V_BYTES_UP(a, 12));
    vec b = v_xor(v_xor(V_LANES_DOWN(lo, 1), V_LANES_DOWN(lo, 2)), V_LANES_DOWN(lo, 7));
    b = v_xor(b, carry);
    return v_xor(hi, v_xor(lo, b));
}

NI vec v_gfmul(vec a, vec b) {
    vec lo = v_zero(), mid = v_zero(), hi = v_zero();
    v_clmul(a, b, &lo, &mid, &hi);
    return v_reduce(lo, mid, hi);
}

NI_FN void gcm_powers_ni(gcm* g) {
    vec h = v_rev(v_load(g->h)), p = h;
    v_store(g->hpow[0], h);
    for (int i = 1; i < 8; i++) {
        p = v_gfmul(p, h);
        v_store(g->hpow[i], p);
    }
}

/* GHASH eight blocks a step: (y + x1) H^8 + x2 H^7 + ... + x8 H, reduced once. */
NI_FN void ghash_ni(const gcm* g, cx_u8 y[16], const cx_u8* p, cx_usize n) {
    vec h[8];
    for (int i = 0; i < 8; i++) h[i] = v_load(g->hpow[i]);
    vec acc = v_rev(v_load(y));
    for (; n >= 128; p += 128, n -= 128) {
        vec lo = v_zero(), mid = v_zero(), hi = v_zero();
        for (int j = 0; j < 8; j++) {
            vec x = v_rev(v_load(p + 16 * j));
            if (j == 0) x = v_xor(x, acc);
            v_clmul(x, h[7 - j], &lo, &mid, &hi);
        }
        acc = v_reduce(lo, mid, hi);
    }
    for (; n >= 16; p += 16, n -= 16) acc = v_gfmul(v_xor(acc, v_rev(v_load(p))), h[0]);
    if (n) {
        cx_u8 last[16] = {0};
        memcpy(last, p, n);
        acc = v_gfmul(v_xor(acc, v_rev(v_load(last))), h[0]);
    }
    v_store(y, v_rev(acc));
}

/* CTR: `out` is `in` xored with the counter blocks from `ctr` on, eight at a time. */
NI_FN void ctr_ni(const gcm* g, const cx_u8 j0[16], cx_u32 ctr, const cx_u8* in, cx_u8* out, cx_usize n) {
    vec k[15];
    int rounds = (int)g->rounds;
    for (int i = 0; i <= rounds; i++) k[i] = v_load(g->rk[i]);
    vec base = v_load(j0);
    vec x[8];
    for (; n >= 128; in += 128, out += 128, n -= 128, ctr += 8) {
        for (int j = 0; j < 8; j++) x[j] = v_counter(base, ctr + (cx_u32)j);
        aes8(x, k, rounds, 8);
        for (int j = 0; j < 8; j++) v_store(out + 16 * j, v_xor(x[j], v_load(in + 16 * j)));
    }
    if (n) {
        int count = (int)((n + 15) / 16);
        for (int j = 0; j < count; j++) x[j] = v_counter(base, ctr + (cx_u32)j);
        aes8(x, k, rounds, count);
        cx_u8 ks[128];
        for (int j = 0; j < count; j++) v_store(ks + 16 * j, x[j]);
        for (cx_usize i = 0; i < n; i++) out[i] = (cx_u8)(in[i] ^ ks[i]);
        wipe(ks, sizeof ks);
    }
    wipe(k, sizeof k);
    wipe(x, sizeof x);
}

NI_FN void block_ni(const gcm* g, const cx_u8 in[16], cx_u8 out[16]) {
    vec k[15];
    int rounds = (int)g->rounds;
    for (int i = 0; i <= rounds; i++) k[i] = v_load(g->rk[i]);
    vec x[8];
    x[0] = v_load(in);
    aes8(x, k, rounds, 1);
    v_store(out, x[0]);
    wipe(k, sizeof k);
    wipe(x, sizeof x);
}
#endif

static void ghash(const gcm* g, cx_u8 y[16], const cx_u8* p, cx_usize n) {
#if defined(CX_X86) || defined(CX_ARM)
    if (g->ni) {
        ghash_ni(g, y, p, n);
        return;
    }
#endif
    ghash_ct(g->h, y, p, n);
}

/* `out` is `in` xored with the key stream from the block after J0, four blocks at a time
 * without the instructions. */
static void gcm_ctr(const gcm* g, const cx_u8 j0[16], const cx_u8* in, cx_u8* out, cx_usize n) {
    cx_u32 ctr = ((cx_u32)j0[12] << 24 | (cx_u32)j0[13] << 16 | (cx_u32)j0[14] << 8 | j0[15]) + 1;
#if defined(CX_X86) || defined(CX_ARM)
    if (g->ni) {
        ctr_ni(g, j0, ctr, in, out, n);
        return;
    }
#endif
    cx_u8 ks[64];
    while (n) {
        for (int b = 0; b < 4; b++, ctr++) {
            memcpy(ks + 16 * b, j0, 12);
            ks[16 * b + 12] = (cx_u8)(ctr >> 24);
            ks[16 * b + 13] = (cx_u8)(ctr >> 16);
            ks[16 * b + 14] = (cx_u8)(ctr >> 8);
            ks[16 * b + 15] = (cx_u8)ctr;
        }
        cx_usize take = n < 64 ? n : 64;
        aes_blocks_ct(g->rk, (int)g->rounds, ks, (int)((take + 15) / 16));
        for (cx_usize i = 0; i < take; i++) out[i] = (cx_u8)(in[i] ^ ks[i]);
        in += take;
        out += take;
        n -= take;
    }
    wipe(ks, sizeof ks);
}

/* One block encrypted, for H and for the tag's mask. */
static void gcm_block(const gcm* g, const cx_u8 in[16], cx_u8 out[16]) {
#if defined(CX_X86) || defined(CX_ARM)
    if (g->ni) {
        block_ni(g, in, out);
        return;
    }
#endif
    memcpy(out, in, 16);
    aes_blocks_ct(g->rk, (int)g->rounds, out, 1);
}

/* J0: the 12-byte IV then the counter 1, or for another length the GHASH of the IV, padded,
 * then its length in bits. */
static void gcm_j0(const gcm* g, const cx_u8* iv, cx_usize ivlen, cx_u8 j0[16]) {
    if (ivlen == 12) {
        memcpy(j0, iv, 12);
        j0[12] = j0[13] = j0[14] = 0;
        j0[15] = 1;
        return;
    }
    cx_u8 lens[16] = {0};
    memset(j0, 0, 16);
    ghash(g, j0, iv, ivlen);
    st64be(lens + 8, (cx_u64)ivlen * 8);
    ghash(g, j0, lens, 16);
}

/* The tag: GHASH of the associated data and the ciphertext, each padded, and their lengths in
 * bits, xored with J0 encrypted. */
static void gcm_tag(const gcm* g, const cx_u8 j0[16], const cx_u8* aad, cx_usize alen, const cx_u8* ct, cx_usize clen, cx_u8 tag[16]) {
    cx_u8 y[16] = {0}, lens[16], mask[16];
    ghash(g, y, aad, alen);
    ghash(g, y, ct, clen);
    st64be(lens, (cx_u64)alen * 8);
    st64be(lens + 8, (cx_u64)clen * 8);
    ghash(g, y, lens, 16);
    gcm_block(g, j0, mask);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ mask[i];
    wipe(mask, sizeof mask);
}

void cx_gcm_start(cx_u8* ctx, const cx_u8* key, cx_usize len) {
    gcm g;
    memset(&g, 0, sizeof g);
    int nk = (int)(len / 4);
    g.rounds = (cx_u32)(nk + 6);
    g.ni = gcm_ni();
    aes_expand(key, nk, g.rk);
    cx_u8 zero[16] = {0};
    gcm_block(&g, zero, g.h);
#if defined(CX_X86) || defined(CX_ARM)
    if (g.ni) gcm_powers_ni(&g);
#endif
    memcpy(ctx, &g, sizeof g);
    wipe(&g, sizeof g);
}

void cx_gcm_seal(const cx_u8* ctx, const cx_u8* iv, cx_usize ivlen, const cx_u8* pt, cx_usize n, const cx_u8* aad, cx_usize alen, cx_u8* out) {
    gcm g;
    cx_u8 j0[16];
    memcpy(&g, ctx, sizeof g);
    gcm_j0(&g, iv, ivlen, j0);
    gcm_ctr(&g, j0, pt, out, n);
    gcm_tag(&g, j0, aad, alen, out, n, out + n);
    wipe(&g, sizeof g);
}

cx_bool cx_gcm_open(const cx_u8* ctx, const cx_u8* iv, cx_usize ivlen, const cx_u8* sealed, cx_usize n, const cx_u8* aad, cx_usize alen, cx_u8* out) {
    if (n < 16) return 0;
    n -= 16;
    gcm g;
    cx_u8 j0[16], tag[16];
    memcpy(&g, ctx, sizeof g);
    gcm_j0(&g, iv, ivlen, j0);
    gcm_tag(&g, j0, aad, alen, sealed, n, tag);
    int ok = same_tag(tag, sealed + n);
    if (ok) gcm_ctr(&g, j0, sealed, out, n);
    wipe(&g, sizeof g);
    return ok != 0;
}
