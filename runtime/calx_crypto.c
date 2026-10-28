/* Random bytes from the system, AES-128 in CFB8, and RSA with PKCS#1 v1.5 padding, behind
 * std.crypto (D118). Linked only into programs that call them. Vanilla's online-mode login is
 * these: the server makes a 1024-bit RSA key pair, the client sends a shared secret encrypted with
 * the public key, and both sides then encrypt everything with AES-CFB8, the secret as key and IV.
 *
 * AES is FIPS 197, with AES-NI where the processor has it and a byte-at-a-time version where it
 * doesn't. RSA is RFC 8017: Montgomery multiplication on 64-bit limbs, exponents taken four bits a
 * step from a table read whole each time, so the time doesn't depend on the private key's bits,
 * and the Chinese remainder theorem for decryption. Contexts are bytes the caller owns, a
 * `List<u8>` in std.crypto, copied in and out here, so they need no alignment. */
#include "calx_rt.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <errno.h>
#include <sys/random.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#define CX_X86 1
#endif

/* --- random bytes -------------------------------------------------------------------------- */

void cx_crypto_random(cx_u8* p, cx_usize n) {
#ifdef _WIN32
    while (n) {
        ULONG take = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        if (BCryptGenRandom(NULL, p, take, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) cx_panic_capacity();
        p += take;
        n -= take;
    }
#else
    while (n) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            cx_panic_capacity();
        }
        p += got;
        n -= (cx_usize)got;
    }
#endif
}

/* --- AES-128 ------------------------------------------------------------------------------- */

typedef struct {
    cx_u8 rk[11][16];  // the round keys
    cx_u8 iv[16];      // the shift register: the last 16 bytes of ciphertext
    cx_u32 ni;         // whether to use AES-NI
} cfb8;

_Static_assert(sizeof(cfb8) <= CX_CFB8_CTX, "the context fits what std.crypto makes room for");

/* The S-box, worked out from its definition: the inverse in GF(2^8), by logarithms to the
 * generator 3, then the affine map. */
static const cx_u8 sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static cx_u8 xtime(cx_u8 x) { return (cx_u8)((x << 1) ^ ((x >> 7) * 0x1b)); }

/* Whether the processor has AES-NI and SSE4.1. */
static cx_u32 aes_ni(void) {
#ifdef CX_X86
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) return (c >> 25) & 1 && (c >> 19) & 1;
#endif
    return 0;
}

static void expand_key(const cx_u8* key, cx_u8 rk[11][16]) {
    static const cx_u8 rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    memcpy(rk[0], key, 16);
    for (int r = 1; r <= 10; r++) {
        const cx_u8* p = rk[r - 1];
        cx_u8 t[4] = {(cx_u8)(sbox[p[13]] ^ rcon[r - 1]), sbox[p[14]], sbox[p[15]], sbox[p[12]]};
        for (int i = 0; i < 4; i++) rk[r][i] = p[i] ^ t[i];
        for (int i = 4; i < 16; i++) rk[r][i] = p[i] ^ rk[r][i - 4];
    }
}

/* One block, without AES-NI. */
static void encrypt_block(const cx_u8 rk[11][16], const cx_u8* in, cx_u8* out) {
    cx_u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[0][i];
    for (int r = 1; r <= 10; r++) {
        cx_u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[(i + 4 * (i & 3)) & 15]];  // SubBytes and ShiftRows
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                cx_u8* w = t + 4 * c;
                cx_u8 a0 = w[0], a1 = w[1], a2 = w[2], a3 = w[3], all = a0 ^ a1 ^ a2 ^ a3;
                w[0] ^= all ^ xtime(a0 ^ a1);
                w[1] ^= all ^ xtime(a1 ^ a2);
                w[2] ^= all ^ xtime(a2 ^ a3);
                w[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[r][i];
    }
    memcpy(out, s, 16);
}

void cx_cfb8_start(cx_u8* ctx, const cx_u8* key, const cx_u8* iv) {
    cfb8 c;
    c.ni = aes_ni();
    expand_key(key, c.rk);
    memcpy(c.iv, iv, 16);
    memcpy(ctx, &c, sizeof c);
}

#ifdef CX_X86
#define AES_TARGET __attribute__((target("aes,sse4.1")))

/* Encrypting is one block per byte, each waiting on the last: the byte out goes into the shift
 * register the next block is made from. */
AES_TARGET static void encrypt_ni(cfb8* c, cx_u8* p, cx_usize n) {
    __m128i k[11];
    for (int i = 0; i < 11; i++) k[i] = _mm_loadu_si128((const __m128i*)c->rk[i]);
    __m128i iv = _mm_loadu_si128((const __m128i*)c->iv);
    for (cx_usize i = 0; i < n; i++) {
        __m128i x = _mm_xor_si128(iv, k[0]);
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
        x = _mm_aesenclast_si128(x, k[10]);
        cx_u8 out = (cx_u8)(p[i] ^ (cx_u8)_mm_cvtsi128_si32(x));
        p[i] = out;
        iv = _mm_insert_epi8(_mm_srli_si128(iv, 1), out, 15);
    }
    _mm_storeu_si128((__m128i*)c->iv, iv);
}

/* Decrypting needs only the ciphertext, which is all there: the block for byte `i` is the 16
 * bytes of ciphertext before it. So 16 bytes go at once, 16 blocks through the rounds together,
 * which keeps the AES unit busy instead of waiting on each round. `w` holds the 16 bytes before a
 * step's and its own, since the step writes plaintext over them. */
AES_TARGET static void decrypt_ni(cfb8* c, cx_u8* p, cx_usize n) {
    __m128i k[11];
    for (int i = 0; i < 11; i++) k[i] = _mm_loadu_si128((const __m128i*)c->rk[i]);
    cx_u8 w[32];
    memcpy(w, c->iv, 16);
    cx_usize i = 0;
    for (; n - i >= 16; i += 16) {
        memcpy(w + 16, p + i, 16);
        __m128i x[16];
        for (int j = 0; j < 16; j++) x[j] = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(w + j)), k[0]);
        for (int r = 1; r < 10; r++) {
            for (int j = 0; j < 16; j++) x[j] = _mm_aesenc_si128(x[j], k[r]);
        }
        for (int j = 0; j < 16; j++) {
            x[j] = _mm_aesenclast_si128(x[j], k[10]);
            p[i + j] = (cx_u8)(w[16 + j] ^ (cx_u8)_mm_cvtsi128_si32(x[j]));
        }
        memcpy(w, w + 16, 16);
    }
    for (; i < n; i++) {
        __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i*)w), k[0]);
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
        x = _mm_aesenclast_si128(x, k[10]);
        cx_u8 in = p[i];
        p[i] = (cx_u8)(in ^ (cx_u8)_mm_cvtsi128_si32(x));
        memmove(w, w + 1, 15);
        w[15] = in;
    }
    memcpy(c->iv, w, 16);
}
#endif

void cx_cfb8_encrypt(cx_u8* ctx, cx_u8* p, cx_usize n) {
    cfb8 c;
    memcpy(&c, ctx, sizeof c);
#ifdef CX_X86
    if (c.ni) {
        encrypt_ni(&c, p, n);
        memcpy(ctx, &c, sizeof c);
        return;
    }
#endif
    for (cx_usize i = 0; i < n; i++) {
        cx_u8 b[16];
        encrypt_block(c.rk, c.iv, b);
        p[i] ^= b[0];
        memmove(c.iv, c.iv + 1, 15);
        c.iv[15] = p[i];
    }
    memcpy(ctx, &c, sizeof c);
}

void cx_cfb8_decrypt(cx_u8* ctx, cx_u8* p, cx_usize n) {
    cfb8 c;
    memcpy(&c, ctx, sizeof c);
#ifdef CX_X86
    if (c.ni) {
        decrypt_ni(&c, p, n);
        memcpy(ctx, &c, sizeof c);
        return;
    }
#endif
    for (cx_usize i = 0; i < n; i++) {
        cx_u8 b[16];
        encrypt_block(c.rk, c.iv, b);
        cx_u8 in = p[i];
        p[i] ^= b[0];
        memmove(c.iv, c.iv + 1, 15);
        c.iv[15] = in;
    }
    memcpy(ctx, &c, sizeof c);
}

/* --- numbers for RSA ----------------------------------------------------------------------- */

typedef unsigned __int128 u128;
#define MAXL 64  // limbs in the largest modulus, 4096 bits, which public keys may have
#define KEYL 32  // limbs in the modulus of the largest key pair made, 2048 bits

/* r = a - b over n limbs; the borrow out. */
static cx_u64 sub_n(cx_u64* r, const cx_u64* a, const cx_u64* b, int n) {
    cx_u64 borrow = 0;
    for (int i = 0; i < n; i++) {
        u128 d = (u128)a[i] - b[i] - borrow;
        r[i] = (cx_u64)d;
        borrow = (cx_u64)(d >> 64) & 1;
    }
    return borrow;
}

static cx_u64 add_n(cx_u64* r, const cx_u64* a, const cx_u64* b, int n) {
    cx_u64 carry = 0;
    for (int i = 0; i < n; i++) {
        u128 s = (u128)a[i] + b[i] + carry;
        r[i] = (cx_u64)s;
        carry = (cx_u64)(s >> 64);
    }
    return carry;
}

/* r = a if `mask` is all ones, else r unchanged. */
static void select_n(cx_u64* r, const cx_u64* a, cx_u64 mask, int n) {
    for (int i = 0; i < n; i++) r[i] = (r[i] & ~mask) | (a[i] & mask);
}

/* r (2n limbs) = a * b (n limbs each). */
static void mul_n(cx_u64* r, const cx_u64* a, const cx_u64* b, int n) {
    memset(r, 0, sizeof(cx_u64) * 2 * (size_t)n);
    for (int i = 0; i < n; i++) {
        cx_u64 carry = 0;
        for (int j = 0; j < n; j++) {
            u128 t = (u128)a[j] * b[i] + r[i + j] + carry;
            r[i + j] = (cx_u64)t;
            carry = (cx_u64)(t >> 64);
        }
        r[i + n] = carry;
    }
}

static int cmp_n(const cx_u64* a, const cx_u64* b, int n) {
    for (int i = n; i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* a mod m, for m below 2^32: 32 bits a step, so nothing needs a 128-bit division, which the
 * Windows target has no library for. */
static cx_u64 mod_small(const cx_u64* a, int n, cx_u64 m) {
    cx_u64 r = 0;
    for (int i = n; i-- > 0;) {
        r = ((r << 32) | (a[i] >> 32)) % m;
        r = ((r << 32) | (a[i] & 0xFFFFFFFFu)) % m;
    }
    return r;
}

/* An odd modulus for Montgomery multiplication, with R = 2^(64n). */
typedef struct {
    int n;
    cx_u64 m[MAXL];
    cx_u64 inv;  // -m^-1 mod 2^64
    cx_u64 rr[MAXL];  // R^2 mod m
} mont;

/* r = a * b / R mod m, for a and b below m: the CIOS method, then a subtraction of m taken or not
 * by a mask, so the time is the same for every value. */
static void mont_mul(const mont* M, cx_u64* r, const cx_u64* a, const cx_u64* b) {
    int n = M->n;
    cx_u64 t[MAXL + 2];
    memset(t, 0, sizeof(cx_u64) * (size_t)(n + 2));
    for (int i = 0; i < n; i++) {
        cx_u64 carry = 0;
        for (int j = 0; j < n; j++) {
            u128 x = (u128)a[j] * b[i] + t[j] + carry;
            t[j] = (cx_u64)x;
            carry = (cx_u64)(x >> 64);
        }
        u128 x = (u128)t[n] + carry;
        t[n] = (cx_u64)x;
        t[n + 1] = (cx_u64)(x >> 64);
        cx_u64 u = t[0] * M->inv;
        x = (u128)u * M->m[0] + t[0];
        carry = (cx_u64)(x >> 64);
        for (int j = 1; j < n; j++) {
            x = (u128)u * M->m[j] + t[j] + carry;
            t[j - 1] = (cx_u64)x;
            carry = (cx_u64)(x >> 64);
        }
        x = (u128)t[n] + carry;
        t[n - 1] = (cx_u64)x;
        t[n] = t[n + 1] + (cx_u64)(x >> 64);
    }
    cx_u64 s[MAXL];
    cx_u64 borrow = sub_n(s, t, M->m, n);
    cx_u64 take = (cx_u64)0 - (cx_u64)(t[n] | (borrow ^ 1));  // t >= m
    memcpy(r, t, sizeof(cx_u64) * (size_t)n);
    select_n(r, s, take, n);
}

/* Sets up an odd modulus of n limbs whose top limb isn't 0. */
static void mont_setup(mont* M, const cx_u64* m, int n) {
    M->n = n;
    memcpy(M->m, m, sizeof(cx_u64) * (size_t)n);
    cx_u64 x = m[0];  // Newton's iteration for m^-1 mod 2^64: each step doubles the bits right
    for (int i = 0; i < 6; i++) x *= 2 - m[0] * x;
    M->inv = (cx_u64)0 - x;
    // R^2 mod m, which is 2^n in Montgomery form, 2^n R, squared 6 times in Montgomery form, as
    // (2^n)^64 = R. 2^n R mod m is the highest power of 2 below m, 2^(b-1), doubled 64n + n - b + 1
    // times, less m each time it reaches m: about n doublings, where 1 doubled 128n times takes
    // 128n.
    int b = 64 * n - __builtin_clzll(m[n - 1]);
    cx_u64 r[MAXL];
    memset(r, 0, sizeof(cx_u64) * (size_t)n);
    r[(b - 1) / 64] = (cx_u64)1 << ((b - 1) % 64);
    for (int i = 0; i < 64 * n + n - b + 1; i++) {
        cx_u64 top = r[n - 1] >> 63;
        for (int j = n - 1; j > 0; j--) r[j] = (r[j] << 1) | (r[j - 1] >> 63);
        r[0] <<= 1;
        cx_u64 s[MAXL];
        cx_u64 borrow = sub_n(s, r, m, n);
        select_n(r, s, (cx_u64)0 - (top | (borrow ^ 1)), n);
    }
    for (int i = 0; i < 6; i++) mont_mul(M, r, r, r);
    memcpy(M->rr, r, sizeof(cx_u64) * (size_t)n);
}

/* r = t / R mod m, for t (2n limbs) below m * R. */
static void mont_reduce(const mont* M, cx_u64* r, const cx_u64* wide) {
    int n = M->n;
    cx_u64 t[2 * MAXL + 1];
    memcpy(t, wide, sizeof(cx_u64) * 2 * (size_t)n);
    t[2 * n] = 0;
    for (int i = 0; i < n; i++) {
        cx_u64 u = t[i] * M->inv, carry = 0;
        for (int j = 0; j < n; j++) {
            u128 x = (u128)u * M->m[j] + t[i + j] + carry;
            t[i + j] = (cx_u64)x;
            carry = (cx_u64)(x >> 64);
        }
        for (int j = i + n; carry && j <= 2 * n; j++) {
            u128 x = (u128)t[j] + carry;
            t[j] = (cx_u64)x;
            carry = (cx_u64)(x >> 64);
        }
    }
    cx_u64 s[MAXL];
    cx_u64 borrow = sub_n(s, t + n, M->m, n);
    memcpy(r, t + n, sizeof(cx_u64) * (size_t)n);
    select_n(r, s, (cx_u64)0 - (t[2 * n] | (borrow ^ 1)), n);
}

/* r = a^e mod m, for a below m, with `bits` bits of e. Four bits a step: four squarings, then a
 * multiply by the table's entry for them, which reads every entry and keeps one by a mask. */
static void mont_pow(const mont* M, cx_u64* r, const cx_u64* a, const cx_u64* e, int bits) {
    int n = M->n;
    static const cx_u64 one_limb[MAXL] = {1};
    cx_u64 table[16][MAXL];
    mont_mul(M, table[1], a, M->rr);  // a * R
    mont_mul(M, table[0], one_limb, M->rr);  // R, which is 1
    for (int i = 2; i < 16; i++) mont_mul(M, table[i], table[i - 1], table[1]);
    cx_u64 x[MAXL];
    memcpy(x, table[0], sizeof x);
    int steps = (bits + 3) / 4;
    for (int s = steps; s-- > 0;) {
        for (int k = 0; k < 4; k++) mont_mul(M, x, x, x);
        int at = 4 * s;
        unsigned w = (unsigned)((e[at / 64] >> (at % 64)) & 15);
        cx_u64 pick[MAXL];
        memset(pick, 0, sizeof(cx_u64) * (size_t)n);
        for (unsigned i = 0; i < 16; i++) select_n(pick, table[i], (cx_u64)0 - (cx_u64)(i == w), n);
        mont_mul(M, x, x, pick);
    }
    mont_mul(M, r, x, one_limb);
}

/* r = a^e mod m, for a below m, with `bits` bits of e, a bit at a time from the top: a squaring
 * for each, and a multiply for each that's set. Its time depends on e, so it's for exponents that
 * aren't secret: a public key's 65537 takes 16 squarings and a multiply, where mont_pow's table
 * takes 46 multiplies in all. A squaring is a multiply: one made apart, with each cross product
 * once and the reduction after, was slower than the multiply's interleaved reduction. */
static void mont_pow_public(const mont* M, cx_u64* r, const cx_u64* a, const cx_u64* e, int bits) {
    static const cx_u64 one_limb[MAXL] = {1};
    int top = bits;
    while (top > 0 && !((e[(top - 1) / 64] >> ((top - 1) % 64)) & 1)) top--;
    cx_u64 base[MAXL], x[MAXL];
    mont_mul(M, base, a, M->rr);  // a * R
    if (top == 0) {
        mont_mul(M, r, M->rr, one_limb);  // a^0 is 1: R, out of Montgomery form
        return;
    }
    memcpy(x, base, sizeof(cx_u64) * (size_t)M->n);
    for (int i = top - 1; i-- > 0;) {
        mont_mul(M, x, x, x);
        if ((e[i / 64] >> (i % 64)) & 1) mont_mul(M, x, x, base);
    }
    mont_mul(M, r, x, one_limb);
}

/* --- RSA ----------------------------------------------------------------------------------- */

typedef struct {
    cx_u32 bits;  // of the modulus, 1024 or 2048
    cx_u32 half;  // limbs in p and q
    cx_u64 n[KEYL];
    cx_u64 p[KEYL / 2], q[KEYL / 2], dp[KEYL / 2], dq[KEYL / 2], qinv[KEYL / 2];
} rsa;

_Static_assert(sizeof(rsa) <= CX_RSA_CTX, "the context fits what std.crypto makes room for");

#define E 65537u

/* a^e mod m, for m below 2^32. */
static cx_u64 pow_small(cx_u64 a, cx_u64 e, cx_u64 m) {
    cx_u64 r = 1;
    for (a %= m; e; e >>= 1, a = a * a % m) {
        if (e & 1) r = r * a % m;
    }
    return r;
}

/* Whether the odd number is probably prime: no small prime divides it, and 16 rounds of
 * Miller-Rabin with random bases pass, which leaves less than a 2^-32 chance per round of a
 * composite slipping through, far less for numbers this big. */
static int probably_prime(const cx_u64* c, int n) {
    cx_u16 primes[300];
    int count = 0;
    for (cx_u32 k = 3; count < 300; k += 2) {
        int prime = 1;
        for (int i = 0; i < count && (cx_u32)primes[i] * primes[i] <= k; i++) {
            if (k % primes[i] == 0) prime = 0;
        }
        if (prime) primes[count++] = (cx_u16)k;
    }
    for (int i = 0; i < count; i++) {
        if (mod_small(c, n, primes[i]) == 0) return 0;
    }
    mont M;
    mont_setup(&M, c, n);
    cx_u64 d[MAXL], minus1[MAXL], one[MAXL] = {1};
    sub_n(minus1, c, one, n);
    memcpy(d, minus1, sizeof d);
    int s = 0;
    while (!(d[0] & 1)) {
        for (int i = 0; i < n - 1; i++) d[i] = (d[i] >> 1) | (d[i + 1] << 63);
        d[n - 1] >>= 1;
        s++;
    }
    for (int round = 0; round < 16; round++) {
        cx_u64 a[MAXL] = {0};
        cx_crypto_random((cx_u8*)a, sizeof(cx_u64) * (size_t)n);
        a[n - 1] &= 0x3FFFFFFFFFFFFFFFull;  // below c, whose top two bits are set
        if (a[0] < 2) a[0] = 2;
        cx_u64 x[MAXL];
        mont_pow(&M, x, a, d, 64 * n);
        if (cmp_n(x, one, n) == 0 || cmp_n(x, minus1, n) == 0) continue;
        int ok = 0;
        for (int k = 1; k < s && !ok; k++) {
            cx_u64 xr[MAXL];
            mont_mul(&M, xr, x, M.rr);  // into Montgomery form, squared, and out
            mont_mul(&M, xr, xr, xr);
            mont_mul(&M, x, xr, one);
            if (cmp_n(x, one, n) == 0) return 0;
            ok = cmp_n(x, minus1, n) == 0;
        }
        if (!ok) return 0;
    }
    return 1;
}

/* A random prime of 64n bits whose top two bits are set, so two make a modulus of exactly 128n
 * bits, and with p - 1 not a multiple of E, so E has an inverse. */
static void random_prime(cx_u64* p, int n) {
    for (;;) {
        cx_crypto_random((cx_u8*)p, sizeof(cx_u64) * (size_t)n);
        p[n - 1] |= 0xC000000000000000ull;
        p[0] |= 1;
        if (mod_small(p, n, E) == 1) continue;
        if (probably_prime(p, n)) return;
    }
}

/* d = E^-1 mod m, for m = p - 1: d = (1 + k m) / E, where k = -m^-1 mod E makes it whole. */
static void inverse_e(cx_u64* d, const cx_u64* m, int n) {
    cx_u64 r = mod_small(m, n, E);
    cx_u64 k = (E - pow_small(r, E - 2, E)) % E;
    cx_u64 t[MAXL / 2 + 1];
    cx_u64 carry = 1;  // the 1
    for (int i = 0; i < n; i++) {
        u128 x = (u128)m[i] * k + carry;
        t[i] = (cx_u64)x;
        carry = (cx_u64)(x >> 64);
    }
    t[n] = carry;
    cx_u64 rem = 0;
    for (int i = n + 1; i-- > 0;) {
        cx_u64 hi = (rem << 32) | (t[i] >> 32);
        rem = hi % E;
        cx_u64 lo = (rem << 32) | (t[i] & 0xFFFFFFFFu);
        rem = lo % E;
        t[i] = (hi / E) << 32 | (lo / E);
    }
    memcpy(d, t, sizeof(cx_u64) * (size_t)n);
}

/* Big-endian bytes to limbs, and back. */
static void from_bytes(cx_u64* r, int n, const cx_u8* b, cx_usize len) {
    memset(r, 0, sizeof(cx_u64) * (size_t)n);
    for (cx_usize i = 0; i < len; i++) {
        cx_usize bit = 8 * (len - 1 - i);
        r[bit / 64] |= (cx_u64)b[i] << (bit % 64);
    }
}

static void to_bytes(cx_u8* b, cx_usize len, const cx_u64* r) {
    for (cx_usize i = 0; i < len; i++) {
        cx_usize bit = 8 * (len - 1 - i);
        b[i] = (cx_u8)(r[bit / 64] >> (bit % 64));
    }
}

/* The rest of a key pair whose bits, half and primes are set: the modulus and the CRT exponents
 * and coefficient. */
static void finish_key(rsa* kp) {
    rsa k = *kp;
    int h = (int)k.half;
    mul_n(k.n, k.p, k.q, h);
    cx_u64 one[MAXL / 2] = {1}, m[MAXL / 2];
    sub_n(m, k.p, one, h);
    inverse_e(k.dp, m, h);
    sub_n(m, k.q, one, h);
    inverse_e(k.dq, m, h);
    // qinv = q^(p-2) mod p, by Fermat, with q first reduced below p.
    mont M;
    mont_setup(&M, k.p, h);
    cx_u64 qr[MAXL / 2], s[MAXL / 2], two[MAXL / 2] = {2};
    memcpy(qr, k.q, sizeof qr);
    if (cmp_n(qr, k.p, h) >= 0) sub_n(qr, qr, k.p, h);
    sub_n(s, k.p, two, h);
    mont_pow(&M, k.qinv, qr, s, 64 * h);
    *kp = k;
}

/* Makes a key pair of 1024 or 2048 bits, with E as the public exponent, as Java's
 * KeyPairGenerator does. Returns 0 for any other size. */
cx_bool cx_rsa_generate(cx_u8* ctx, cx_u32 bits) {
    if (bits != 1024 && bits != 2048) return 0;
    rsa k;
    memset(&k, 0, sizeof k);
    k.bits = bits;
    int h = (int)(bits / 128);
    k.half = (cx_u32)h;
    random_prime(k.p, h);
    do random_prime(k.q, h);
    while (cmp_n(k.p, k.q, h) == 0);
    finish_key(&k);
    memcpy(ctx, &k, sizeof k);
    return 1;
}

/* The key pair's primes, p then q, each half the modulus's bytes, big-endian, which is all
 * `cx_rsa_from_primes` needs to make it again. Writes them to `out`, which has room for 256
 * bytes, and returns their length: the modulus's. */
cx_usize cx_rsa_primes(const cx_u8* ctx, cx_u8* out) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    cx_usize hb = k.bits / 16;
    to_bytes(out, hb, k.p);
    to_bytes(out + hb, hb, k.q);
    return 2 * hb;
}

/* The key pair whose primes `cx_rsa_primes` gave: `len` is 128 or 256 bytes, p then q. Returns 0
 * unless they're two different primes as `cx_rsa_generate` makes them: their top two bits set,
 * and neither one more than a multiple of E. */
cx_bool cx_rsa_from_primes(cx_u8* ctx, const cx_u8* in, cx_usize len) {
    if (len != 128 && len != 256) return 0;
    rsa k;
    memset(&k, 0, sizeof k);
    k.bits = (cx_u32)(8 * len);
    int h = (int)(len / 16);
    k.half = (cx_u32)h;
    from_bytes(k.p, h, in, len / 2);
    from_bytes(k.q, h, in + len / 2, len / 2);
    cx_u64* both[2] = {k.p, k.q};
    for (int i = 0; i < 2; i++) {
        cx_u64* p = both[i];
        if ((p[h - 1] >> 62) != 3 || !(p[0] & 1) || mod_small(p, h, E) == 1 || !probably_prime(p, h)) return 0;
    }
    if (cmp_n(k.p, k.q, h) == 0) return 0;
    finish_key(&k);
    memcpy(ctx, &k, sizeof k);
    return 1;
}

/* The modulus's length in bytes. */
cx_usize cx_rsa_size(const cx_u8* ctx) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    return k.bits / 8;
}

/* DER: a length, and an INTEGER of big-endian bytes with a 0 before them if the top bit is set. */
static cx_usize der_len(cx_u8* o, cx_usize n) {
    if (n < 128) {
        if (o) o[0] = (cx_u8)n;
        return 1;
    }
    cx_usize bytes = n < 256 ? 1 : 2;
    if (o) {
        o[0] = (cx_u8)(0x80 | bytes);
        for (cx_usize i = 0; i < bytes; i++) o[1 + i] = (cx_u8)(n >> (8 * (bytes - 1 - i)));
    }
    return 1 + bytes;
}

/* The public key as Java's `getEncoded()` gives it, an X.509 SubjectPublicKeyInfo: the RSA
 * algorithm's identifier, then the modulus and exponent in a bit string. Writes it to `out`,
 * which has room for 300 bytes, and returns its length. */
cx_usize cx_rsa_public_der(const cx_u8* ctx, cx_u8* out) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    cx_usize nb = k.bits / 8;
    cx_u8 modulus[257];
    modulus[0] = 0;
    to_bytes(modulus + 1, nb, k.n);  // the top bit is always set, so the 0 always goes first
    static const cx_u8 exponent[] = {0x02, 0x03, 0x01, 0x00, 0x01};
    static const cx_u8 algorithm[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
    cx_usize int_len = 1 + der_len(0, nb + 1) + nb + 1;
    cx_usize seq_len = int_len + sizeof exponent;
    cx_usize seq_all = 1 + der_len(0, seq_len) + seq_len;
    cx_usize bit_len = 1 + seq_all;
    cx_usize body = sizeof algorithm + 1 + der_len(0, bit_len) + bit_len;
    cx_usize o = 0;
    out[o++] = 0x30;
    o += der_len(out + o, body);
    memcpy(out + o, algorithm, sizeof algorithm);
    o += sizeof algorithm;
    out[o++] = 0x03;
    o += der_len(out + o, bit_len);
    out[o++] = 0x00;  // no unused bits
    out[o++] = 0x30;
    o += der_len(out + o, seq_len);
    out[o++] = 0x02;
    o += der_len(out + o, nb + 1);
    memcpy(out + o, modulus, nb + 1);
    o += nb + 1;
    memcpy(out + o, exponent, sizeof exponent);
    o += sizeof exponent;
    return o;
}

/* Encrypts `len` bytes, at most the key's bytes less 11, with random padding: what the client
 * does with the server's key. Writes the key's bytes to `out` and returns 1, or returns 0 if the
 * message is too long. */
cx_bool cx_rsa_encrypt(const cx_u8* ctx, const cx_u8* msg, cx_usize len, cx_u8* out) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    cx_usize nb = k.bits / 8;
    if (len + 11 > nb) return 0;
    cx_u8 em[256];
    em[0] = 0;
    em[1] = 2;
    cx_usize ps = nb - 3 - len;
    cx_crypto_random(em + 2, ps);
    for (cx_usize i = 0; i < ps; i++) {
        while (!em[2 + i]) cx_crypto_random(em + 2 + i, 1);  // the padding has no zero byte
    }
    em[2 + ps] = 0;
    memcpy(em + 3 + ps, msg, len);
    int n = (int)(nb / 8);
    mont M;
    mont_setup(&M, k.n, n);
    cx_u64 m[MAXL], c[MAXL], e[MAXL] = {E};
    from_bytes(m, n, em, nb);
    mont_pow(&M, c, m, e, 17);
    to_bytes(out, nb, c);
    return 1;
}

/* m = c^d mod pq, by the Chinese remainder theorem: c^dp mod p and c^dq mod q, combined. */
static void private_op(const rsa* k, cx_u64* m, const cx_u64* c) {
    int h = (int)k->half;
    mont P, Q;
    mont_setup(&P, k->p, h);
    mont_setup(&Q, k->q, h);
    cx_u64 cp[MAXL / 2], cq[MAXL / 2], m1[MAXL / 2], m2[MAXL / 2];
    // c mod p: c / R mod p, then times R^2 / R.
    mont_reduce(&P, cp, c);
    mont_mul(&P, cp, cp, P.rr);
    mont_reduce(&Q, cq, c);
    mont_mul(&Q, cq, cq, Q.rr);
    mont_pow(&P, m1, cp, k->dp, 64 * h);
    mont_pow(&Q, m2, cq, k->dq, 64 * h);
    // h = qinv (m1 - m2) mod p, with m2 first reduced below p.
    cx_u64 m2p[MAXL / 2], s[MAXL / 2], diff[MAXL / 2], t[MAXL / 2];
    memcpy(m2p, m2, sizeof m2p);
    cx_u64 borrow = sub_n(s, m2p, k->p, h);
    select_n(m2p, s, (cx_u64)0 - (borrow ^ 1), h);
    borrow = sub_n(diff, m1, m2p, h);
    add_n(s, diff, k->p, h);
    select_n(diff, s, (cx_u64)0 - borrow, h);
    mont_mul(&P, t, diff, k->qinv);
    mont_mul(&P, t, t, P.rr);
    // m = m2 + h q.
    cx_u64 hq[MAXL], m2w[MAXL] = {0};
    mul_n(hq, t, k->q, h);
    memcpy(m2w, m2, sizeof(cx_u64) * (size_t)h);
    add_n(m, hq, m2w, 2 * h);
}

/* Decrypts a message the public key encrypted, as Java's "RSA/ECB/PKCS1Padding" does. `c` is
 * the key's bytes long. Writes the message to `out`, which has room for the key's bytes, and
 * returns its length, or -1 if the padding is wrong. The padding is checked in the same time
 * whatever is wrong with it. */
cx_i64 cx_rsa_decrypt(const cx_u8* ctx, const cx_u8* in, cx_usize len, cx_u8* out) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    cx_usize nb = k.bits / 8;
    int n = (int)(nb / 8);
    if (len != nb) return -1;
    cx_u64 c[MAXL], m[MAXL];
    from_bytes(c, n, in, nb);
    if (cmp_n(c, k.n, n) >= 0) return -1;
    private_op(&k, m, c);
    cx_u8 em[256];
    to_bytes(em, nb, m);
    // 0, 2, at least eight bytes that aren't 0, a 0, then the message.
    cx_u32 bad = em[0] | (em[1] ^ 2);
    cx_u32 found = 0;
    cx_usize at = 0;
    for (cx_usize i = 2; i < nb; i++) {
        cx_u32 zero = em[i] == 0;
        cx_u32 first = zero & (found ^ 1);
        at |= (cx_usize)((cx_usize)0 - first) & i;
        found |= zero;
    }
    bad |= found ^ 1;
    bad |= at < 10;
    if (bad) return -1;
    cx_usize mlen = nb - at - 1;
    memcpy(out, em + at + 1, mlen);
    return (cx_i64)mlen;
}

/* The private operation alone, c^d mod n, with no padding: `in` is the key's bytes long, and is
 * read as a number below the modulus. Writes the key's bytes to `out` and returns 1, or returns 0
 * if the length is wrong or the number isn't below the modulus. It's what a PKCS#1 v1.5 signature
 * is, of the padded digest. */
cx_bool cx_rsa_private_op(const cx_u8* ctx, const cx_u8* in, cx_usize len, cx_u8* out) {
    rsa k;
    memcpy(&k, ctx, sizeof k);
    cx_usize nb = k.bits / 8;
    int n = (int)(nb / 8);
    if (len != nb) return 0;
    cx_u64 c[MAXL], m[MAXL];
    from_bytes(c, n, in, nb);
    if (cmp_n(c, k.n, n) >= 0) return 0;
    private_op(&k, m, c);
    to_bytes(out, nb, m);
    return 1;
}

/* A public key, set up once for its operations. */
typedef struct {
    cx_u32 len;  // the modulus's bytes
    cx_u32 ebits;
    cx_u64 e[MAXL];
    mont M;
} rsa_public;

_Static_assert(sizeof(rsa_public) <= CX_RSA_PUBLIC_CTX, "the context fits what std.crypto makes room for");

/* Sets up a public key in `ctx`: `mod` is the modulus's `len` big-endian bytes, at most 512, the
 * first not 0, and the modulus is odd; `e` is the exponent's `elen` bytes, at most `len`. Returns
 * 0 if any of that doesn't hold. */
cx_bool cx_rsa_public_setup(cx_u8* ctx, const cx_u8* mod, cx_usize len, const cx_u8* e, cx_usize elen) {
    if (len == 0 || len > 8 * MAXL || elen == 0 || elen > len || mod[0] == 0 || !(mod[len - 1] & 1)) return 0;
    rsa_public k;
    memset(&k, 0, sizeof k);
    int n = (int)((len + 7) / 8);
    k.len = (cx_u32)len;
    k.ebits = (cx_u32)(8 * elen);
    cx_u64 m[MAXL];
    from_bytes(m, n, mod, len);
    from_bytes(k.e, n, e, elen);
    mont_setup(&k.M, m, n);
    memcpy(ctx, &k, sizeof k);
    return 1;
}

/* The key's operation, s^e mod n, as a signature is checked: `in` is the modulus's bytes long.
 * Writes as many to `out` and returns 1, or returns 0 if `in` isn't below the modulus. Nothing in
 * it is secret, so its time may depend on the values. */
cx_bool cx_rsa_public_op(const cx_u8* ctx, const cx_u8* in, cx_u8* out) {
    const rsa_public* k = (const rsa_public*)(const void*)ctx;
    rsa_public copy;
    if ((cx_usize)ctx % _Alignof(rsa_public) != 0) {
        memcpy(&copy, ctx, sizeof copy);
        k = &copy;
    }
    int n = k->M.n;
    cx_u64 x[MAXL], r[MAXL];
    from_bytes(x, n, in, k->len);
    if (cmp_n(x, k->M.m, n) >= 0) return 0;
    mont_pow_public(&k->M, r, x, k->e, (int)k->ebits);
    to_bytes(out, k->len, r);
    return 1;
}
