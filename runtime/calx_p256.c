/* ECDSA over NIST P-256 with SHA-256, JWS's ES256, behind std.crypto (CLX-406). Linked only into
 * programs that call it. SHA-256 and HMAC-SHA256, which it hashes messages and makes nonces with,
 * are in calx_digest.c.
 *
 * Numbers mod p, the field's prime, and mod n, the group's order, are four 64-bit limbs in
 * Montgomery form, x R mod m for R = 2^256, multiplied by word-by-word Montgomery reduction
 * through 128-bit products. Every sum, difference and product ends with a subtraction of the
 * modulus that always happens and is kept or dropped by a mask, so nothing branches on a number's
 * value. Inverses are powers, x^(m - 2), whose exponents are public, so the same squarings and
 * multiplies happen whatever x is.
 *
 * Points are projective (X:Y:Z), x = X/Z and y = Y/Z, with the neutral point (0:1:0), and are
 * summed and doubled by Renes, Costello and Batina's complete formulas for a = -3: one formula
 * for every pair of points, the neutral point and a point with itself or its negative among
 * them, so neither secret points nor a verifier's sums of public ones need a special case.
 *
 * Signing multiplies the base point by its secret nonce four bits at a time from a table of the
 * point's first 16 multiples, each entry picked by reading all 16 under masks, so neither the
 * time nor the memory touched depends on the nonce, and nor does making a public key. The nonce
 * is RFC 6979's, from HMAC-SHA256 of the key and the message's hash, so the same key and message
 * always sign the same, and no random source is needed. Verifying works with what's public, so
 * it picks entries straight from tables of G's and Q's multiples, adding both as it doubles. */
#include "calx_rt.h"

#include <string.h>

typedef unsigned __int128 u128;
typedef cx_u64 num[4];

typedef struct {
    num m;          // the modulus
    cx_u64 m0;      // -1/m mod 2^64
    num rr;         // R^2 mod m, which a multiply by turns a number into Montgomery form
} modulus;

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1, and n, little-endian limbs. */
static const modulus P = {
    {0xffffffffffffffffULL, 0x00000000ffffffffULL, 0x0000000000000000ULL, 0xffffffff00000001ULL},
    1,
    {0x0000000000000003ULL, 0xfffffffbffffffffULL, 0xfffffffffffffffeULL, 0x00000004fffffffdULL},
};
static const modulus N = {
    {0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL, 0xffffffffffffffffULL, 0xffffffff00000000ULL},
    0xccd1c8aaee00bc4fULL,
    {0x83244c95be79eea2ULL, 0x4699799c49bd6fa6ULL, 0x2845b2392b6bec59ULL, 0x66e12d94f3d95620ULL},
};

/* 1, b and the base point G's coordinates, in Montgomery form mod p, and (p + 1) / 4, the power
 * that's a square root, since p is 3 mod 4. */
static const num ONE = {0x0000000000000001ULL, 0xffffffff00000000ULL, 0xffffffffffffffffULL, 0x00000000fffffffeULL};
static const num B = {0xd89cdf6229c4bddfULL, 0xacf005cd78843090ULL, 0xe5a220abf7212ed6ULL, 0xdc30061d04874834ULL};
static const num GX = {0x79e730d418a9143cULL, 0x75ba95fc5fedb601ULL, 0x79fb732b77622510ULL, 0x18905f76a53755c6ULL};
static const num GY = {0xddf25357ce95560aULL, 0x8b4ab8e4ba19e45cULL, 0xd2e88688dd21f325ULL, 0x8571ff1825885d85ULL};
static const num SQRT = {0x0000000000000000ULL, 0x0000000040000000ULL, 0x4000000000000000ULL, 0x3fffffffc0000000ULL};

/* --- numbers ------------------------------------------------------------------------------- */

static void wipe(void* p, cx_usize n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

static void load(num r, const cx_u8* b) {
    for (int i = 0; i < 4; i++) {
        cx_u64 w = 0;
        for (int j = 0; j < 8; j++) w = w << 8 | b[8 * i + j];
        r[3 - i] = w;
    }
}

static void store(cx_u8* b, const num a) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) b[8 * i + j] = (cx_u8)(a[3 - i] >> (56 - 8 * j));
    }
}

/* 1 if a < m, from the borrow of a - m. */
static cx_u64 below(const num a, const num m) {
    cx_u64 borrow = 0;
    for (int i = 0; i < 4; i++) borrow = (cx_u64)(((u128)a[i] - m[i] - borrow) >> 64) & 1;
    return borrow;
}

/* 1 if a is 0, without a branch. */
static cx_u64 is_zero(const num a) {
    cx_u64 any = a[0] | a[1] | a[2] | a[3];
    return ((any | (0 - any)) >> 63) ^ 1;
}

static cx_u64 equal(const num a, const num b) {
    num d;
    for (int i = 0; i < 4; i++) d[i] = a[i] ^ b[i];
    return is_zero(d);
}

/* a, with a fifth limb `hi`, less m if that doesn't go below 0. The subtraction always happens,
 * and a mask keeps it or not. */
static void reduce_once(num r, const num a, cx_u64 hi, const num m) {
    num d;
    cx_u64 borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - m[i] - borrow;
        d[i] = (cx_u64)t;
        borrow = (cx_u64)(t >> 64) & 1;
    }
    cx_u64 keep = 0 - ((cx_u64)(((u128)hi - borrow) >> 64) & 1);  // all ones when a was below m
    for (int i = 0; i < 4; i++) r[i] = (a[i] & keep) | (d[i] & ~keep);
}

static void mod_add(num r, const num a, const num b, const num m) {
    num s;
    cx_u64 c = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] + b[i] + c;
        s[i] = (cx_u64)t;
        c = (cx_u64)(t >> 64);
    }
    reduce_once(r, s, c, m);
}

/* a - b, with m added back under a mask when it went below 0. */
static void mod_sub(num r, const num a, const num b, const num m) {
    num d;
    cx_u64 borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - b[i] - borrow;
        d[i] = (cx_u64)t;
        borrow = (cx_u64)(t >> 64) & 1;
    }
    cx_u64 mask = 0 - borrow, c = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)d[i] + (m[i] & mask) + c;
        r[i] = (cx_u64)t;
        c = (cx_u64)(t >> 64);
    }
}

/* a b / R mod m, for a and b below m: each limb of b adds a b[i], then a multiple of m that
 * clears the low limb, which is shifted out. What's held stays below 2m, so one subtraction at
 * the end brings it below m. r can be a or b. */
static void mont_mul(num r, const num a, const num b, const modulus* M) {
    cx_u64 t[6] = {0};
    for (int i = 0; i < 4; i++) {
        cx_u64 c = 0;
        for (int j = 0; j < 4; j++) {
            u128 x = (u128)a[j] * b[i] + t[j] + c;
            t[j] = (cx_u64)x;
            c = (cx_u64)(x >> 64);
        }
        u128 x = (u128)t[4] + c;
        t[4] = (cx_u64)x;
        t[5] = (cx_u64)(x >> 64);
        cx_u64 q = t[0] * M->m0;
        x = (u128)q * M->m[0] + t[0];
        c = (cx_u64)(x >> 64);
        for (int j = 1; j < 4; j++) {
            x = (u128)q * M->m[j] + t[j] + c;
            t[j - 1] = (cx_u64)x;
            c = (cx_u64)(x >> 64);
        }
        x = (u128)t[4] + c;
        t[3] = (cx_u64)x;
        t[4] = t[5] + (cx_u64)(x >> 64);
    }
    reduce_once(r, t, t[4], M->m);
}

/* mont_mul for p, whose shape makes the reduction one multiply a limb, not four: -1/p mod 2^64
 * is 1, so the multiple of p added is q = t[0] times it. q (2^64 - 1) clears t[0] and carries
 * q, q (2^32 - 1) plus that carry is q 2^32, p's third limb is 0, and only its top limb,
 * 2^64 - 2^32 + 1, takes a multiply. */
static void fe_mul(num r, const num a, const num b) {
    cx_u64 t[6] = {0};
    for (int i = 0; i < 4; i++) {
        cx_u64 c = 0;
        for (int j = 0; j < 4; j++) {
            u128 x = (u128)a[j] * b[i] + t[j] + c;
            t[j] = (cx_u64)x;
            c = (cx_u64)(x >> 64);
        }
        u128 x = (u128)t[4] + c;
        t[4] = (cx_u64)x;
        t[5] = (cx_u64)(x >> 64);
        cx_u64 q = t[0];
        x = (u128)t[1] + ((u128)q << 32);
        t[0] = (cx_u64)x;
        x = (u128)t[2] + (cx_u64)(x >> 64);
        t[1] = (cx_u64)x;
        x = (u128)t[3] + (u128)q * P.m[3] + (cx_u64)(x >> 64);
        t[2] = (cx_u64)x;
        x = (u128)t[4] + (cx_u64)(x >> 64);
        t[3] = (cx_u64)x;
        t[4] = t[5] + (cx_u64)(x >> 64);
    }
    reduce_once(r, t, t[4], P.m);
}

static void fe_add(num r, const num a, const num b) { mod_add(r, a, b, P.m); }
static void fe_sub(num r, const num a, const num b) { mod_sub(r, a, b, P.m); }

static void mul(num r, const num a, const num b, const modulus* M) {
    if (M == &P) {
        fe_mul(r, a, b);
    } else {
        mont_mul(r, a, b, M);
    }
}

static void to_mont(num r, const num a, const modulus* M) { mul(r, a, M->rr, M); }

static void from_mont(num r, const num a, const modulus* M) {
    static const num one = {1, 0, 0, 0};
    mul(r, a, one, M);
}

/* a^e for a public e, in Montgomery form, four bits of e at a time from the top: four squarings
 * each, and a multiply by a^0 to a^15 from a table for each four that aren't all 0. e is public,
 * so which entry is read, and whether, can depend on it. */
static void mod_pow(num r, const num a, const num e, const modulus* M) {
    static const num one = {1, 0, 0, 0};
    num table[16], acc;
    to_mont(table[0], one, M);
    memcpy(table[1], a, sizeof(num));
    for (int i = 2; i < 16; i++) mul(table[i], table[i - 1], a, M);
    memcpy(acc, table[0], sizeof acc);
    for (int i = 63; i >= 0; i--) {
        for (int j = 0; j < 4; j++) mul(acc, acc, acc, M);
        int w = (int)(e[i / 16] >> (4 * (i % 16))) & 15;
        if (w) mul(acc, acc, table[w], M);
    }
    memcpy(r, acc, sizeof acc);
    wipe(table, sizeof table);
}

/* 1/a mod m, as a^(m - 2), in Montgomery form. 0 has none, and gives 0. */
static void mod_inv(num r, const num a, const modulus* M) {
    num e;
    memcpy(e, M->m, sizeof e);
    e[0] -= 2;  // neither modulus's low limb is below 2
    mod_pow(r, a, e, M);
}

/* x^3 - 3x + b, which is y^2 for a point on the curve. */
static void curve_rhs(num r, const num x) {
    num x3, t;
    fe_mul(x3, x, x);
    fe_mul(x3, x3, x);
    fe_add(t, x, x);
    fe_add(t, t, x);
    fe_sub(x3, x3, t);
    fe_add(r, x3, B);
}

/* --- points -------------------------------------------------------------------------------- */

typedef struct {
    num X, Y, Z;
} point;

static void point_zero(point* p) {
    memset(p->X, 0, sizeof(num));
    memcpy(p->Y, ONE, sizeof(num));
    memset(p->Z, 0, sizeof(num));
}

static void point_base(point* p) {
    memcpy(p->X, GX, sizeof(num));
    memcpy(p->Y, GY, sizeof(num));
    memcpy(p->Z, ONE, sizeof(num));
}

/* Renes, Costello and Batina's algorithm 4, the complete sum for a = -3: 12 multiplies, 2 of
 * them by b. r can be p or q. */
static void point_add(point* r, const point* p, const point* q) {
    num t0, t1, t2, t3, t4, x3, y3, z3;
    fe_mul(t0, p->X, q->X);
    fe_mul(t1, p->Y, q->Y);
    fe_mul(t2, p->Z, q->Z);
    fe_add(t3, p->X, p->Y);
    fe_add(t4, q->X, q->Y);
    fe_mul(t3, t3, t4);
    fe_add(t4, t0, t1);
    fe_sub(t3, t3, t4);
    fe_add(t4, p->Y, p->Z);
    fe_add(x3, q->Y, q->Z);
    fe_mul(t4, t4, x3);
    fe_add(x3, t1, t2);
    fe_sub(t4, t4, x3);
    fe_add(x3, p->X, p->Z);
    fe_add(y3, q->X, q->Z);
    fe_mul(x3, x3, y3);
    fe_add(y3, t0, t2);
    fe_sub(y3, x3, y3);
    fe_mul(z3, B, t2);
    fe_sub(x3, y3, z3);
    fe_add(z3, x3, x3);
    fe_add(x3, x3, z3);
    fe_sub(z3, t1, x3);
    fe_add(x3, t1, x3);
    fe_mul(y3, B, y3);
    fe_add(t1, t2, t2);
    fe_add(t2, t1, t2);
    fe_sub(y3, y3, t2);
    fe_sub(y3, y3, t0);
    fe_add(t1, y3, y3);
    fe_add(y3, t1, y3);
    fe_add(t1, t0, t0);
    fe_add(t0, t1, t0);
    fe_sub(t0, t0, t2);
    fe_mul(t1, t4, y3);
    fe_mul(t2, t0, y3);
    fe_mul(y3, x3, z3);
    fe_add(y3, y3, t2);
    fe_mul(x3, t3, x3);
    fe_sub(x3, x3, t1);
    fe_mul(z3, t4, z3);
    fe_mul(t1, t3, t0);
    fe_add(z3, z3, t1);
    memcpy(r->X, x3, sizeof(num));
    memcpy(r->Y, y3, sizeof(num));
    memcpy(r->Z, z3, sizeof(num));
}

/* Their algorithm 6, the doubling for a = -3: 8 multiplies and 3 squarings. r can be p. */
static void point_double(point* r, const point* p) {
    num t0, t1, t2, t3, x3, y3, z3;
    fe_mul(t0, p->X, p->X);
    fe_mul(t1, p->Y, p->Y);
    fe_mul(t2, p->Z, p->Z);
    fe_mul(t3, p->X, p->Y);
    fe_add(t3, t3, t3);
    fe_mul(z3, p->X, p->Z);
    fe_add(z3, z3, z3);
    fe_mul(y3, B, t2);
    fe_sub(y3, y3, z3);
    fe_add(x3, y3, y3);
    fe_add(y3, x3, y3);
    fe_sub(x3, t1, y3);
    fe_add(y3, t1, y3);
    fe_mul(y3, x3, y3);
    fe_mul(x3, x3, t3);
    fe_add(t3, t2, t2);
    fe_add(t2, t2, t3);
    fe_mul(z3, B, z3);
    fe_sub(z3, z3, t2);
    fe_sub(z3, z3, t0);
    fe_add(t3, z3, z3);
    fe_add(z3, z3, t3);
    fe_add(t3, t0, t0);
    fe_add(t0, t3, t0);
    fe_sub(t0, t0, t2);
    fe_mul(t0, t0, z3);
    fe_add(y3, y3, t0);
    fe_mul(t0, p->Y, p->Z);
    fe_add(t0, t0, t0);
    fe_mul(z3, t0, z3);
    fe_sub(x3, x3, z3);
    fe_mul(z3, t0, t1);
    fe_add(z3, z3, z3);
    fe_add(z3, z3, z3);
    memcpy(r->X, x3, sizeof(num));
    memcpy(r->Y, y3, sizeof(num));
    memcpy(r->Z, z3, sizeof(num));
}

/* x and y, out of Montgomery form, of a point that isn't the neutral one. */
static void point_affine(num x, num y, const point* p) {
    num zi;
    mod_inv(zi, p->Z, &P);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    from_mont(x, x, &P);
    from_mont(y, y, &P);
}

/* The point at x and y, 32 bytes each, big-endian, or 0 unless both are below p and the point
 * is on the curve. With a cofactor of 1, every such point is in the group. */
static int point_load(point* p, const cx_u8* xy) {
    num x, y, rhs, y2;
    load(x, xy);
    load(y, xy + 32);
    if (!(below(x, P.m) & below(y, P.m))) return 0;
    to_mont(p->X, x, &P);
    to_mont(p->Y, y, &P);
    memcpy(p->Z, ONE, sizeof(num));
    curve_rhs(rhs, p->X);
    fe_mul(y2, p->Y, p->Y);
    return (int)equal(rhs, y2);
}

/* The first 16 multiples of p, 0 to 15 times it. */
static void point_table(point table[16], const point* p) {
    point_zero(&table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; i++) {
        if (i % 2 == 0) {
            point_double(&table[i], &table[i / 2]);
        } else {
            point_add(&table[i], &table[i - 1], p);
        }
    }
}

/* Bits 4i to 4i + 3 from the top of a 32-byte big-endian scalar. */
static int nibble(const cx_u8* k, int i) { return (k[i / 2] >> (i & 1 ? 0 : 4)) & 15; }

/* Sets r to a where `flag` is 1, by masks, so the time is the same either way. */
static void point_cmov(point* r, const point* a, cx_u64 flag) {
    cx_u64 m = 0 - flag;
    for (int i = 0; i < 4; i++) {
        r->X[i] ^= m & (r->X[i] ^ a->X[i]);
        r->Y[i] ^= m & (r->Y[i] ^ a->Y[i]);
        r->Z[i] ^= m & (r->Z[i] ^ a->Z[i]);
    }
}

/* [k]G for a secret k, four bits at a time from the top. Each window's entry is picked by
 * reading all 16 under masks. */
static void base_mul(point* r, const cx_u8* k) {
    point table[16], pick, g;
    point_base(&g);
    point_table(table, &g);
    point_zero(r);
    for (int i = 0; i < 64; i++) {
        if (i > 0) {
            for (int j = 0; j < 4; j++) point_double(r, r);
        }
        cx_u64 want = (cx_u64)nibble(k, i);
        pick = table[0];
        for (cx_u64 j = 1; j < 16; j++) point_cmov(&pick, &table[j], ((j ^ want) - 1) >> 63);
        point_add(r, r, &pick);
    }
    wipe(table, sizeof table);
    wipe(&pick, sizeof pick);
}

/* [a]G + [b]Q, for public a and b. The sums are complete, so a step that lands on the neutral
 * point, or adds a point to itself, needs nothing of its own. */
static void double_mul(point* r, const cx_u8* a, const cx_u8* b, const point* q) {
    point tg[16], tq[16], g;
    point_base(&g);
    point_table(tg, &g);
    point_table(tq, q);
    point_zero(r);
    for (int i = 0; i < 64; i++) {
        if (i > 0) {
            for (int j = 0; j < 4; j++) point_double(r, r);
        }
        point_add(r, r, &tg[nibble(a, i)]);
        point_add(r, r, &tq[nibble(b, i)]);
    }
}

/* --- keys and signatures ------------------------------------------------------------------- */

static void sha256(cx_u8 out[32], const cx_u8* msg, cx_usize len) {
    cx_u8 ctx[CX_DIGEST_CTX];
    cx_digest_start(ctx, 2);
    if (len) cx_digest_add(ctx, msg, len);
    cx_digest_finish(ctx, out);
}

/* A hash as a number mod n. It's 256 bits, n's length, so all of it is used, and it's below 2n,
 * so one subtraction reduces it: RFC 6979's bits2int, then mod n, which its bits2octets is too. */
static void hash_to_scalar(num e, const cx_u8 h[32]) {
    load(e, h);
    reduce_once(e, e, 0, N.m);
}

cx_bool cx_p256_scalar_check(const cx_u8* d) {
    num k;
    load(k, d);
    cx_bool ok = (cx_bool)(below(k, N.m) & (is_zero(k) ^ 1));
    wipe(k, sizeof k);
    return ok;
}

/* Q = [d]G, as x then y, 32 bytes each. d must be from 1 to n - 1. */
void cx_p256_public(const cx_u8* d, cx_u8* out) {
    point q;
    num x, y;
    base_mul(&q, d);
    point_affine(x, y, &q);
    store(out, x);
    store(out + 32, y);
    wipe(&q, sizeof q);
}

cx_bool cx_p256_check(const cx_u8* xy) {
    point q;
    return point_load(&q, xy);
}

/* y for x and y's parity, from y^2 = x^3 - 3x + b by the power (p + 1) / 4, or 0 if x isn't
 * below p or that's no square, so no point has that x. */
cx_bool cx_p256_decompress(const cx_u8* x, cx_bool odd, cx_u8* y_out) {
    num xn, xm, rhs, ym, y2, y;
    load(xn, x);
    if (!below(xn, P.m)) return 0;
    to_mont(xm, xn, &P);
    curve_rhs(rhs, xm);
    mod_pow(ym, rhs, SQRT, &P);
    fe_mul(y2, ym, ym);
    if (!equal(y2, rhs)) return 0;
    from_mont(y, ym, &P);
    if ((int)(y[0] & 1) != (odd ? 1 : 0)) {
        static const num zero = {0, 0, 0, 0};
        if (is_zero(y)) return 0;
        mod_sub(y, zero, y, P.m);
    }
    store(y_out, y);
    return 1;
}

/* r = ([k]G).x mod n and s = (e + r d) / k mod n for each of RFC 6979's nonces k in turn, until
 * neither is 0, which one in 2^256 or so isn't. The nonces come from HMAC-SHA256 keyed by K over
 * V, both worked out from d and the hash. */
void cx_p256_sign(const cx_u8* d, const cx_u8* msg, cx_usize len, cx_u8* out) {
    cx_u8 h[32], K[32], V[32], buf[97], kbytes[32];
    num e, dn, em, dm, k, km, kinv, r, rm, s, x, y;
    point R;
    sha256(h, msg, len);
    hash_to_scalar(e, h);
    load(dn, d);

    // RFC 6979's 3.2, steps b to f: V = 1s, K = 0s, then K = HMAC_K(V || 0 || x || h) and V =
    // HMAC_K(V), then again with 1, where x is the key and h the hash mod n, 32 bytes each.
    memset(V, 1, 32);
    memset(K, 0, 32);
    memcpy(buf + 33, d, 32);
    store(buf + 65, e);
    for (int round = 0; round < 2; round++) {
        memcpy(buf, V, 32);
        buf[32] = (cx_u8)round;
        cx_digest_hmac_sha256(K, 32, buf, 97, K);
        cx_digest_hmac_sha256(K, 32, V, 32, V);
    }

    to_mont(em, e, &N);
    to_mont(dm, dn, &N);
    for (int first = 1;; first = 0) {
        // Step h: after a k that was out of range or gave an r or s of 0, K = HMAC_K(V || 0) and
        // V = HMAC_K(V) first. Then V = HMAC_K(V), which is k if it's from 1 to n - 1.
        if (!first) {
            memcpy(buf, V, 32);
            buf[32] = 0;
            cx_digest_hmac_sha256(K, 32, buf, 33, K);
            cx_digest_hmac_sha256(K, 32, V, 32, V);
        }
        cx_digest_hmac_sha256(K, 32, V, 32, V);
        memcpy(kbytes, V, 32);
        load(k, kbytes);
        if (!(below(k, N.m) & (is_zero(k) ^ 1))) continue;

        base_mul(&R, kbytes);
        point_affine(x, y, &R);
        reduce_once(r, x, 0, N.m);  // x is below p, which is below 2n
        if (is_zero(r)) continue;

        to_mont(km, k, &N);
        mod_inv(kinv, km, &N);
        to_mont(rm, r, &N);
        mont_mul(s, rm, dm, &N);
        mod_add(s, s, em, N.m);
        mont_mul(s, s, kinv, &N);
        from_mont(s, s, &N);
        if (is_zero(s)) continue;
        store(out, r);
        store(out + 32, s);
        break;
    }
    wipe(K, sizeof K);
    wipe(V, sizeof V);
    wipe(buf, sizeof buf);
    wipe(kbytes, sizeof kbytes);
    wipe(dn, sizeof dn);
    wipe(dm, sizeof dm);
    wipe(k, sizeof k);
    wipe(km, sizeof km);
    wipe(kinv, sizeof kinv);
    wipe(y, sizeof y);
    wipe(&R, sizeof R);
}

/* r and s from 1 to n - 1, then w = 1/s, and ([e w]G + [r w]Q).x mod n must be r. */
cx_bool cx_p256_verify(const cx_u8* xy, const cx_u8* msg, cx_usize len, const cx_u8* sig) {
    cx_u8 h[32], u1b[32], u2b[32];
    num r, s, e, sm, w, u1, u2, t, x, y;
    point q, R;
    load(r, sig);
    load(s, sig + 32);
    if (is_zero(r) || is_zero(s) || !below(r, N.m) || !below(s, N.m)) return 0;
    if (!point_load(&q, xy)) return 0;
    sha256(h, msg, len);
    hash_to_scalar(e, h);
    to_mont(sm, s, &N);
    mod_inv(w, sm, &N);
    to_mont(t, e, &N);
    mont_mul(u1, t, w, &N);
    to_mont(t, r, &N);
    mont_mul(u2, t, w, &N);
    from_mont(u1, u1, &N);
    from_mont(u2, u2, &N);
    store(u1b, u1);
    store(u2b, u2);
    double_mul(&R, u1b, u2b, &q);
    if (is_zero(R.Z)) return 0;
    point_affine(x, y, &R);
    reduce_once(x, x, 0, N.m);
    return (cx_bool)equal(x, r);
}
