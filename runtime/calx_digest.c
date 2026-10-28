/* MD5, SHA-1 and SHA-256, and HMAC-SHA256, behind std.hash (D118). Linked only into programs
 * that call them. Written from RFC 1321, FIPS 180-4 and RFC 2104; the constants are computed from
 * their definitions: MD5's from sines, SHA-256's from the cube and square roots of the first
 * primes. Vanilla hashes a name with MD5 for an offline player's UUID, the seed with SHA-256 for
 * the join packet, and the login's secret and key with SHA-1 for the session server.
 *
 * A context is CX_DIGEST_CTX bytes the caller owns, a `List<u8>` in std.hash, so they're copied
 * in and out of a struct here and need no alignment. */
#include "calx_rt.h"

#include <string.h>

enum { MD5, SHA1, SHA256 };

typedef struct {
    cx_u32 h[8];
    cx_u64 len;  // bytes added
    cx_u8 buf[64];
    cx_u32 kind;
    cx_u32 used;  // bytes waiting in `buf`
} digest;

_Static_assert(sizeof(digest) <= CX_DIGEST_CTX, "the context fits what std.hash makes room for");

static inline cx_u32 rotl(cx_u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static inline cx_u32 rotr(cx_u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static inline cx_u32 load_le(const cx_u8* p) { return (cx_u32)p[0] | (cx_u32)p[1] << 8 | (cx_u32)p[2] << 16 | (cx_u32)p[3] << 24; }
static inline cx_u32 load_be(const cx_u8* p) { return (cx_u32)p[0] << 24 | (cx_u32)p[1] << 16 | (cx_u32)p[2] << 8 | (cx_u32)p[3]; }

/* --- MD5 ----------------------------------------------------------------------------------- */

static const cx_u32 md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const cx_u8 md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(cx_u32* h, const cx_u8* p) {
    cx_u32 m[16];
    for (int i = 0; i < 16; i++) m[i] = load_le(p + 4 * i);
    cx_u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        cx_u32 f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else f = c ^ (b | ~d), g = (7 * i) & 15;
        cx_u32 t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + md5_k[i] + m[g], md5_r[i]);
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

/* --- SHA-1 --------------------------------------------------------------------------------- */

static void sha1_block(cx_u32* h, const cx_u8* p) {
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

static void sha256_block(cx_u32* h, const cx_u8* p) {
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

/* --- the three, through one context -------------------------------------------------------- */

static void block(digest* d, const cx_u8* p) {
    switch (d->kind) {
        case MD5: md5_block(d->h, p); break;
        case SHA1: sha1_block(d->h, p); break;
        default: sha256_block(d->h, p); break;
    }
}

static void start(digest* d, cx_u32 kind) {
    static const cx_u32 md5_h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const cx_u32 sha1_h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    static const cx_u32 sha256_h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(d, 0, sizeof *d);
    d->kind = kind;
    if (kind == MD5) memcpy(d->h, md5_h, sizeof md5_h);
    else if (kind == SHA1) memcpy(d->h, sha1_h, sizeof sha1_h);
    else memcpy(d->h, sha256_h, sizeof sha256_h);
}

static void add(digest* d, const cx_u8* p, cx_usize n) {
    d->len += n;
    if (d->used) {
        cx_usize take = 64 - d->used < n ? 64 - d->used : n;
        memcpy(d->buf + d->used, p, take);
        d->used += (cx_u32)take;
        p += take;
        n -= take;
        if (d->used < 64) return;
        block(d, d->buf);
        d->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(d, p);
    memcpy(d->buf, p, n);
    d->used = (cx_u32)n;
}

/* Pads with a 1 bit, zeros and the length in bits, little-endian for MD5 and big-endian for the
 * others, and writes the digest. Returns its length. */
static cx_usize finish(digest* d, cx_u8* out) {
    cx_u64 bits = d->len * 8;
    cx_u8 pad[72] = {0x80};
    cx_usize zeros = (d->used < 56 ? 56 : 120) - d->used;
    for (int i = 0; i < 8; i++) pad[zeros + i] = (cx_u8)(d->kind == MD5 ? bits >> (8 * i) : bits >> (56 - 8 * i));
    cx_u64 len = d->len;
    add(d, pad, zeros + 8);
    d->len = len;
    cx_usize words = d->kind == MD5 ? 4 : d->kind == SHA1 ? 5 : 8;
    for (cx_usize i = 0; i < words; i++) {
        cx_u32 v = d->h[i];
        if (d->kind == MD5) out[4 * i] = (cx_u8)v, out[4 * i + 1] = (cx_u8)(v >> 8), out[4 * i + 2] = (cx_u8)(v >> 16), out[4 * i + 3] = (cx_u8)(v >> 24);
        else out[4 * i] = (cx_u8)(v >> 24), out[4 * i + 1] = (cx_u8)(v >> 16), out[4 * i + 2] = (cx_u8)(v >> 8), out[4 * i + 3] = (cx_u8)v;
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
