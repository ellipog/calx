/* Ed25519 signatures, RFC 8032, behind std.crypto (CLX-379). Linked only into programs that call
 * them. SHA-512, which it hashes keys and messages with, is in calx_digest.c.
 *
 * Numbers mod p = 2^255 - 19 are five 51-bit limbs, multiplied through 128-bit products, with the
 * top limb's carry folded back in as 19 times itself. Points are extended coordinates (X:Y:Z:T),
 * x = X/Z, y = Y/Z and xy = T/Z, whose sums and doublings are complete on this curve: one formula
 * for every pair, the neutral point and a point with itself among them. Scalars mod L, the
 * group's order, are four 64-bit limbs, reduced a bit at a time with a subtraction that's kept
 * or not by a mask.
 *
 * Signing multiplies the base point by secret scalars, four bits at a time from a table of its
 * first 16 multiples, each entry picked by reading all 16 under masks, so neither the time nor
 * the memory touched depends on the scalar. Verifying works with what's public, so it picks
 * entries straight from tables of B's and -A's multiples, adding both as it doubles. It decides
 * as OpenSSL and Go do: the signature's S must be below L, A and R must be points in their one
 * encoding, and [S]B - [k]A must encode to R's bytes.
 *
 * X25519, RFC 7748's Diffie-Hellman on the same curve in its Montgomery form, is at the end, on
 * the same numbers mod p (CLX-411). */
#include "calx_rt.h"

#include <string.h>

typedef unsigned __int128 u128;
typedef cx_u64 fe[5];

static const cx_u64 MASK51 = ((cx_u64)1 << 51) - 1;

/* --- numbers mod p ------------------------------------------------------------------------- */

static void fe_copy(fe h, const fe f) { memcpy(h, f, sizeof(fe)); }
static void fe_0(fe h) { memset(h, 0, sizeof(fe)); }
static void fe_1(fe h) {
    fe_0(h);
    h[0] = 1;
}

/* Carries each limb's bits above 51 into the next, and the top's into the bottom times 19. */
static void fe_carry(fe h) {
    cx_u64 c;
    c = h[0] >> 51, h[0] &= MASK51, h[1] += c;
    c = h[1] >> 51, h[1] &= MASK51, h[2] += c;
    c = h[2] >> 51, h[2] &= MASK51, h[3] += c;
    c = h[3] >> 51, h[3] &= MASK51, h[4] += c;
    c = h[4] >> 51, h[4] &= MASK51, h[0] += 19 * c;
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
    fe_carry(h);
}

/* f - g, with 4p added so no limb goes below 0 for limbs up to 2^53. */
static void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0x1FFFFFFFFFFFB4 - g[0];
    for (int i = 1; i < 5; i++) h[i] = f[i] + 0x1FFFFFFFFFFFFC - g[i];
    fe_carry(h);
}

static void fe_neg(fe h, const fe f) {
    fe z;
    fe_0(z);
    fe_sub(h, z, f);
}

static void fe_mul(fe h, const fe f, const fe g) {
    cx_u64 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    cx_u64 g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    cx_u64 g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;
    u128 r0 = (u128)f0 * g0 + (u128)f1 * g4_19 + (u128)f2 * g3_19 + (u128)f3 * g2_19 + (u128)f4 * g1_19;
    u128 r1 = (u128)f0 * g1 + (u128)f1 * g0 + (u128)f2 * g4_19 + (u128)f3 * g3_19 + (u128)f4 * g2_19;
    u128 r2 = (u128)f0 * g2 + (u128)f1 * g1 + (u128)f2 * g0 + (u128)f3 * g4_19 + (u128)f4 * g3_19;
    u128 r3 = (u128)f0 * g3 + (u128)f1 * g2 + (u128)f2 * g1 + (u128)f3 * g0 + (u128)f4 * g4_19;
    u128 r4 = (u128)f0 * g4 + (u128)f1 * g3 + (u128)f2 * g2 + (u128)f3 * g1 + (u128)f4 * g0;
    r1 += (cx_u64)(r0 >> 51);
    r2 += (cx_u64)(r1 >> 51);
    r3 += (cx_u64)(r2 >> 51);
    r4 += (cx_u64)(r3 >> 51);
    cx_u64 c = (cx_u64)(r4 >> 51);
    h[0] = ((cx_u64)r0 & MASK51) + 19 * c;
    h[1] = (cx_u64)r1 & MASK51;
    h[2] = (cx_u64)r2 & MASK51;
    h[3] = (cx_u64)r3 & MASK51;
    h[4] = (cx_u64)r4 & MASK51;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
}

static void fe_sq(fe h, const fe f) {
    cx_u64 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    cx_u64 f0_2 = 2 * f0, f1_2 = 2 * f1, f3_19 = 19 * f3, f4_19 = 19 * f4;
    u128 r0 = (u128)f0 * f0 + (u128)f1_2 * f4_19 + (u128)(2 * f2) * f3_19;
    u128 r1 = (u128)f0_2 * f1 + (u128)(2 * f2) * f4_19 + (u128)f3 * f3_19;
    u128 r2 = (u128)f0_2 * f2 + (u128)f1 * f1 + (u128)(2 * f3) * f4_19;
    u128 r3 = (u128)f0_2 * f3 + (u128)f1_2 * f2 + (u128)f4 * f4_19;
    u128 r4 = (u128)f0_2 * f4 + (u128)f1_2 * f3 + (u128)f2 * f2;
    r1 += (cx_u64)(r0 >> 51);
    r2 += (cx_u64)(r1 >> 51);
    r3 += (cx_u64)(r2 >> 51);
    r4 += (cx_u64)(r3 >> 51);
    cx_u64 c = (cx_u64)(r4 >> 51);
    h[0] = ((cx_u64)r0 & MASK51) + 19 * c;
    h[1] = (cx_u64)r1 & MASK51;
    h[2] = (cx_u64)r2 & MASK51;
    h[3] = (cx_u64)r3 & MASK51;
    h[4] = (cx_u64)r4 & MASK51;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
}

static void fe_sqn(fe h, const fe f, int n) {
    fe_sq(h, f);
    while (--n > 0) fe_sq(h, h);
}

/* The 255 bits, little-endian; the top bit of the last byte is left out. */
static void fe_frombytes(fe h, const cx_u8* s) {
    cx_u64 w[4];
    memcpy(w, s, 32);
    h[0] = w[0] & MASK51;
    h[1] = (w[0] >> 51 | w[1] << 13) & MASK51;
    h[2] = (w[1] >> 38 | w[2] << 26) & MASK51;
    h[3] = (w[2] >> 25 | w[3] << 39) & MASK51;
    h[4] = (w[3] >> 12) & MASK51;
}

/* The number's one encoding, below p: after carrying, it's below 2^255 + a little, so it's p or
 * more exactly when adding 19 carries out of bit 255. */
static void fe_tobytes(cx_u8* s, const fe f) {
    fe h;
    fe_copy(h, f);
    fe_carry(h);
    fe_carry(h);
    cx_u64 q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    cx_u64 c;
    c = h[0] >> 51, h[0] &= MASK51, h[1] += c;
    c = h[1] >> 51, h[1] &= MASK51, h[2] += c;
    c = h[2] >> 51, h[2] &= MASK51, h[3] += c;
    c = h[3] >> 51, h[3] &= MASK51, h[4] += c;
    h[4] &= MASK51;
    cx_u64 w[4] = {h[0] | h[1] << 51, h[1] >> 13 | h[2] << 38, h[2] >> 26 | h[3] << 25, h[3] >> 39 | h[4] << 12};
    memcpy(s, w, 32);
}

static int fe_isnegative(const fe f) {
    cx_u8 s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

static int fe_iszero(const fe f) {
    cx_u8 s[32];
    fe_tobytes(s, f);
    cx_u8 any = 0;
    for (int i = 0; i < 32; i++) any |= s[i];
    return any == 0;
}

static int fe_equal(const fe f, const fe g) {
    fe d;
    fe_sub(d, f, g);
    return fe_iszero(d);
}

/* z^(2^250 - 1), and z^11, which both of the powers below are built from. */
static void fe_pow250(fe out, fe z11, const fe z) {
    fe t0, t1, t2, t3;
    fe_sq(t0, z);           // 2
    fe_sqn(t1, t0, 2);      // 8
    fe_mul(t1, z, t1);      // 9
    fe_mul(t0, t0, t1);     // 11
    fe_copy(z11, t0);
    fe_sq(t2, t0);          // 22
    fe_mul(t1, t1, t2);     // 31 = 2^5 - 1
    fe_sqn(t2, t1, 5);
    fe_mul(t1, t2, t1);     // 2^10 - 1
    fe_sqn(t2, t1, 10);
    fe_mul(t2, t2, t1);     // 2^20 - 1
    fe_sqn(t3, t2, 20);
    fe_mul(t2, t3, t2);     // 2^40 - 1
    fe_sqn(t2, t2, 10);
    fe_mul(t1, t2, t1);     // 2^50 - 1
    fe_sqn(t2, t1, 50);
    fe_mul(t2, t2, t1);     // 2^100 - 1
    fe_sqn(t3, t2, 100);
    fe_mul(t2, t3, t2);     // 2^200 - 1
    fe_sqn(t2, t2, 50);
    fe_mul(out, t2, t1);    // 2^250 - 1
}

/* 1/z, as z^(p - 2) = z^(2^255 - 21). */
static void fe_invert(fe out, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 5);        // 2^255 - 32
    fe_mul(out, t, z11);
}

/* z^((p - 5) / 8) = z^(2^252 - 3), the square root's main part. */
static void fe_pow22523(fe out, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 2);        // 2^252 - 4
    fe_mul(out, t, z);
}

/* Sets h to f where `flag` is 1, by masks, so the time is the same either way. */
static void fe_cmov(fe h, const fe f, cx_u64 flag) {
    cx_u64 m = (cx_u64)0 - flag;
    for (int i = 0; i < 5; i++) h[i] ^= m & (h[i] ^ f[i]);
}

/* d = -121665/121666, 2d, and a square root of -1, 2^((p - 1) / 4). */
static const fe D = {929955233495203, 466365720129213, 1662059464998953, 2033849074728123, 1442794654840575};
static const fe D2 = {1859910466990425, 932731440258426, 1072319116312658, 1815898335770999, 633789495995903};
static const fe SQRTM1 = {1718705420411056, 234908883556509, 2233514472574048, 2117202627021982, 765476049583133};

/* --- points -------------------------------------------------------------------------------- */

typedef struct {
    fe X, Y, Z, T;
} point;

/* The base point B: y = 4/5 and x the even root. */
static const point BASE = {
    {1738742601995546, 1146398526822698, 2070867633025821, 562264141797630, 587772402128613},
    {1801439850948184, 1351079888211148, 450359962737049, 900719925474099, 1801439850948198},
    {1, 0, 0, 0, 0},
    {1841354044333475, 16398895984059, 755974180946558, 900171276175154, 1821297809914039},
};

static void point_zero(point* p) {
    fe_0(p->X);
    fe_1(p->Y);
    fe_1(p->Z);
    fe_0(p->T);
}

/* add-2008-hwcd-3, for a = -1: 9 multiplies. */
static void point_add(point* r, const point* p, const point* q) {
    fe a, b, c, d, e, f, g, h, t;
    fe_sub(a, p->Y, p->X);
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);
    fe_add(b, p->Y, p->X);
    fe_add(t, q->Y, q->X);
    fe_mul(b, b, t);
    fe_mul(c, p->T, q->T);
    fe_mul(c, c, D2);
    fe_mul(d, p->Z, q->Z);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

/* dbl-2008-hwcd for a = -1, with every sign turned, which leaves the result the same. T isn't
 * read, and is only worked out when `with_t` says the next step is a sum. */
static void point_double(point* r, const point* p, int with_t) {
    fe a, b, c, e, f, g, h;
    fe_sq(a, p->X);
    fe_sq(b, p->Y);
    fe_sq(c, p->Z);
    fe_add(c, c, c);
    fe_add(h, a, b);
    fe_add(e, p->X, p->Y);
    fe_sq(e, e);
    fe_sub(e, h, e);
    fe_sub(g, a, b);
    fe_add(f, c, g);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    if (with_t) fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

static void point_neg(point* r, const point* p) {
    fe_neg(r->X, p->X);
    fe_copy(r->Y, p->Y);
    fe_copy(r->Z, p->Z);
    fe_neg(r->T, p->T);
}

/* y, with the top bit x's sign, the lowest bit of x. */
static void point_encode(cx_u8* s, const point* p) {
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(s, y);
    s[31] ^= (cx_u8)(fe_isnegative(x) << 7);
}

/* RFC 8032's 5.1.3: y below p, then x from x^2 = (y^2 - 1) / (d y^2 + 1) by one power, times
 * sqrt(-1) if that gives the root of -x^2, and none if neither. x = 0 with the sign bit set is
 * no point either. */
static int point_decode(point* p, const cx_u8* s) {
    cx_u8 back[32];
    fe u, v, v3, vx2, t;
    fe_frombytes(p->Y, s);
    fe_tobytes(back, p->Y);
    for (int i = 0; i < 31; i++) {
        if (back[i] != s[i]) return 0;
    }
    if (back[31] != (s[31] & 0x7F)) return 0;
    fe_1(p->Z);
    fe_sq(u, p->Y);
    fe_mul(v, u, D);
    fe_sub(u, u, p->Z);
    fe_add(v, v, p->Z);
    fe_sq(v3, v);
    fe_mul(v3, v3, v);                // v^3
    fe_sq(t, v3);
    fe_mul(t, t, v);
    fe_mul(t, t, u);                  // u v^7
    fe_pow22523(t, t);
    fe_mul(t, t, v3);
    fe_mul(p->X, t, u);               // u v^3 (u v^7)^((p - 5) / 8)
    fe_sq(vx2, p->X);
    fe_mul(vx2, vx2, v);
    if (!fe_equal(vx2, u)) {
        fe_neg(t, u);
        if (!fe_equal(vx2, t)) return 0;
        fe_mul(p->X, p->X, SQRTM1);
    }
    int sign = s[31] >> 7;
    if (fe_iszero(p->X) && sign) return 0;
    if (fe_isnegative(p->X) != sign) fe_neg(p->X, p->X);
    fe_mul(p->T, p->X, p->Y);
    return 1;
}

/* The first 16 multiples of p, 0 to 15 times it. */
static void point_table(point table[16], const point* p) {
    point_zero(&table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; i++) point_add(&table[i], &table[i - 1], p);
}

static int nibble(const cx_u8* s, int i) { return (s[i / 2] >> (4 * (i & 1))) & 15; }

/* [s]B for a secret s of 32 bytes, little-endian, taken four bits at a time from the top. */
static void base_mul(point* r, const cx_u8* s) {
    point table[16], pick;
    point_table(table, &BASE);
    point_zero(r);
    for (int i = 63; i >= 0; i--) {
        for (int k = 0; k < 4; k++) point_double(r, r, k == 3);
        cx_u64 want = (cx_u64)nibble(s, i);
        pick = table[0];
        for (cx_u64 j = 1; j < 16; j++) {
            cx_u64 hit = ((j ^ want) - 1) >> 63;  // 1 when j is want, without a branch
            fe_cmov(pick.X, table[j].X, hit);
            fe_cmov(pick.Y, table[j].Y, hit);
            fe_cmov(pick.Z, table[j].Z, hit);
            fe_cmov(pick.T, table[j].T, hit);
        }
        point_add(r, r, &pick);
    }
}

/* [a]B + [b]P, for public a and b. */
static void double_mul(point* r, const cx_u8* a, const cx_u8* b, const point* p) {
    point tb[16], tp[16];
    point_table(tb, &BASE);
    point_table(tp, p);
    point_zero(r);
    for (int i = 63; i >= 0; i--) {
        for (int k = 0; k < 4; k++) point_double(r, r, k == 3);
        point_add(r, r, &tb[nibble(a, i)]);
        point_add(r, r, &tp[nibble(b, i)]);
    }
}

/* --- scalars mod L --------------------------------------------------------------------------- */

/* L = 2^252 + 27742317777372353535851937790883648493, little-endian limbs. */
static const cx_u64 L[4] = {0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0, 0x1000000000000000ULL};

/* A number of `n` 64-bit limbs mod L: shifted in a bit at a time from the top, with L taken off
 * whenever what's held reaches it. What's held stays below L, so after the shift it's below 2L,
 * under 2^254, and four limbs hold it. The subtraction always happens; a mask keeps it or not. */
static void mod_l(cx_u64 out[4], const cx_u64* in, int n) {
    cx_u64 r[4] = {0, 0, 0, 0};
    for (int bit = 64 * n - 1; bit >= 0; bit--) {
        cx_u64 in_bit = in[bit / 64] >> (bit % 64) & 1;
        r[3] = r[3] << 1 | r[2] >> 63;
        r[2] = r[2] << 1 | r[1] >> 63;
        r[1] = r[1] << 1 | r[0] >> 63;
        r[0] = r[0] << 1 | in_bit;
        cx_u64 t[4], borrow = 0;
        for (int i = 0; i < 4; i++) {
            u128 d = (u128)r[i] - L[i] - borrow;
            t[i] = (cx_u64)d;
            borrow = (cx_u64)(d >> 64) & 1;
        }
        cx_u64 keep = borrow - 1;  // all ones when r was L or more
        for (int i = 0; i < 4; i++) r[i] = (t[i] & keep) | (r[i] & ~keep);
    }
    memcpy(out, r, sizeof r);
}

static void scalar_from64(cx_u64 out[4], const cx_u8* h) {
    cx_u64 w[8];
    memcpy(w, h, 64);
    mod_l(out, w, 8);
}

/* (r + k a) mod L: the 512-bit product, the sum, then the reduction. */
static void scalar_muladd(cx_u8* s, const cx_u64 r[4], const cx_u64 k[4], const cx_u64 a[4]) {
    cx_u64 w[8] = {0};
    for (int i = 0; i < 4; i++) {
        cx_u64 carry = 0;
        for (int j = 0; j < 4; j++) {
            u128 t = (u128)k[i] * a[j] + w[i + j] + carry;
            w[i + j] = (cx_u64)t;
            carry = (cx_u64)(t >> 64);
        }
        w[i + 4] = carry;
    }
    cx_u64 carry = 0;
    for (int i = 0; i < 8; i++) {
        u128 t = (u128)w[i] + (i < 4 ? r[i] : 0) + carry;
        w[i] = (cx_u64)t;
        carry = (cx_u64)(t >> 64);
    }
    cx_u64 out[4];
    mod_l(out, w, 8);
    memcpy(s, out, 32);
}

static int scalar_below_l(const cx_u8* s) {
    cx_u64 w[4];
    memcpy(w, s, 32);
    for (int i = 3; i >= 0; i--) {
        if (w[i] != L[i]) return w[i] < L[i];
    }
    return 0;
}

/* --- keys and signatures ------------------------------------------------------------------- */

static void wipe(void* p, cx_usize n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

static void sha512(cx_u8* out, const cx_u8* a, cx_usize alen, const cx_u8* b, cx_usize blen, const cx_u8* c, cx_usize clen) {
    cx_u8 ctx[CX_DIGEST_CTX];
    cx_digest_start(ctx, 3);
    if (alen) cx_digest_add(ctx, a, alen);
    if (blen) cx_digest_add(ctx, b, blen);
    if (clen) cx_digest_add(ctx, c, clen);
    cx_digest_finish(ctx, out);
    wipe(ctx, sizeof ctx);
}

/* The seed's SHA-512: the first half, with its low three bits and top bit cleared and the bit
 * below the top set, is the secret scalar; the second half seeds each signature's nonce. */
static void expand(cx_u8 h[64], const cx_u8* seed) {
    sha512(h, seed, 32, NULL, 0, NULL, 0);
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;
}

void cx_ed25519_public(const cx_u8* seed, cx_u8* out) {
    cx_u8 h[64];
    point a;
    expand(h, seed);
    base_mul(&a, h);
    point_encode(out, &a);
    wipe(h, sizeof h);
    wipe(&a, sizeof a);
}

/* R = [r]B for r the hash of the nonce's half and the message, then S = r + k a for k the hash
 * of R, A and the message. `pub` is the seed's public key, which std.crypto keeps beside it: one
 * that isn't would give signatures that give the key away. */
void cx_ed25519_sign(const cx_u8* seed, const cx_u8* pub, const cx_u8* msg, cx_usize len, cx_u8* out) {
    cx_u8 h[64], digest[64], rbytes[32], ra[64];
    cx_u64 r[4], k[4], a[4];
    point p;
    expand(h, seed);
    sha512(digest, h + 32, 32, msg, len, NULL, 0);
    scalar_from64(r, digest);
    memcpy(rbytes, r, 32);
    base_mul(&p, rbytes);
    point_encode(out, &p);
    memcpy(ra, out, 32);
    memcpy(ra + 32, pub, 32);
    sha512(digest, ra, 64, msg, len, NULL, 0);
    scalar_from64(k, digest);
    memcpy(a, h, 32);
    scalar_muladd(out + 32, r, k, a);
    wipe(h, sizeof h);
    wipe(r, sizeof r);
    wipe(rbytes, sizeof rbytes);
    wipe(a, sizeof a);
    wipe(digest, sizeof digest);
    wipe(&p, sizeof p);
}

cx_bool cx_ed25519_check(const cx_u8* pub) {
    point a;
    return point_decode(&a, pub);
}

cx_bool cx_ed25519_verify(const cx_u8* pub, const cx_u8* msg, cx_usize len, const cx_u8* sig) {
    point a, r;
    cx_u8 ra[64], digest[64], kbytes[32], check[32];
    cx_u64 k[4];
    if (!scalar_below_l(sig + 32)) return 0;
    if (!point_decode(&a, pub)) return 0;
    point_neg(&a, &a);
    memcpy(ra, sig, 32);
    memcpy(ra + 32, pub, 32);
    sha512(digest, ra, 64, msg, len, NULL, 0);
    scalar_from64(k, digest);
    memcpy(kbytes, k, 32);
    double_mul(&r, sig + 32, kbytes, &a);
    point_encode(check, &r);
    cx_u8 diff = 0;
    for (int i = 0; i < 32; i++) diff |= check[i] ^ sig[i];
    return diff == 0;
}

/* --- X25519 -------------------------------------------------------------------------------- */

/* RFC 7748's X25519 (CLX-411), Diffie-Hellman on the Montgomery form of the same curve, in the
 * same numbers mod p. The scalar is clamped: its low three bits and top bit cleared and the bit
 * below the top set. The point is u alone, its top bit left out, and a u of p or more is taken
 * mod p, as the RFC says. Montgomery's ladder runs over all 255 bits whatever they are, its two
 * points swapped or not by masks, so neither the time nor the memory touched depends on the
 * scalar, and 1/z is a power, by Ed25519's chain. */

/* Swaps f and g where `flag` is 1, by masks. */
static void fe_cswap(fe f, fe g, cx_u64 flag) {
    cx_u64 m = (cx_u64)0 - flag;
    for (int i = 0; i < 5; i++) {
        cx_u64 t = m & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

/* (A + 2) / 4 for A = 486662, the curve's own number. */
static const fe A24 = {121665, 0, 0, 0, 0};

cx_bool cx_x25519(const cx_u8* scalar, const cx_u8* u, cx_u8* out) {
    cx_u8 k[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb;
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    fe_frombytes(x1, u);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);
    cx_u64 swap = 0;
    for (int t = 254; t >= 0; t--) {
        cx_u64 bit = (cx_u64)(k[t >> 3] >> (t & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(x3, da, cb);
        fe_sq(x3, x3);
        fe_sub(z3, da, cb);
        fe_sq(z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul(z2, A24, e);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, e);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    cx_u8 any = 0;
    for (int i = 0; i < 32; i++) any |= out[i];
    wipe(k, sizeof k);
    wipe(x2, sizeof x2);
    wipe(z2, sizeof z2);
    wipe(x3, sizeof x3);
    wipe(z3, sizeof z3);
    wipe(aa, sizeof aa);
    wipe(bb, sizeof bb);
    wipe(e, sizeof e);
    return any != 0;
}

void cx_x25519_public(const cx_u8* scalar, cx_u8* out) {
    static const cx_u8 nine[32] = {9};
    cx_x25519(scalar, nine, out);
}
