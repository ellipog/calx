/* LZ4 as lz4-java's LZ4BlockOutputStream writes it and LZ4BlockInputStream reads it, behind
 * std.lz4 (D118): what vanilla stores region-file chunks as when server.properties says
 * region-file-compression=lz4. Linked only into programs that call it. Written from the LZ4 block
 * format and the XXH32 spec, and checked against lz4-java 1.10.1, the version vanilla ships.
 *
 * The stream is blocks of up to 64 KiB of the data, each after a 21-byte header: "LZ4Block", a
 * byte that's the method (0x10 stored, 0x20 LZ4) plus the log of the block size less 10, then the
 * compressed length, the length, and the XXH32 of the data with lz4-java's seed, in its low 28
 * bits, each little-endian. A block that LZ4 doesn't make smaller is stored. A header with both
 * lengths 0 ends the stream.
 *
 * A block in LZ4 is sequences: a token, whose high 4 bits are the number of literals and low 4
 * the match's length less 4, 15 meaning more bytes follow, each adding up to 255; the literals;
 * the match's distance back, 2 bytes; and the rest of its length. The last sequence is literals
 * only, the last 5 bytes are always literals, and the compressor starts no match in the last 12. */
#include "calx_rt.h"

#include <stdlib.h>
#include <string.h>

static inline cx_u32 load32(const cx_u8* p) {
    cx_u32 v;
    memcpy(&v, p, 4);
    return v;
}
static inline cx_u64 load64(const cx_u8* p) {
    cx_u64 v;
    memcpy(&v, p, 8);
    return v;
}
static inline void store64(cx_u8* p, cx_u64 v) { memcpy(p, &v, 8); }
static inline void copy16(cx_u8* to, const cx_u8* from) { memcpy(to, from, 16); }
static inline void put32(cx_u8* p, cx_u32 v) {
    p[0] = (cx_u8)v, p[1] = (cx_u8)(v >> 8), p[2] = (cx_u8)(v >> 16), p[3] = (cx_u8)(v >> 24);
}
static inline cx_u32 rotl(cx_u32 v, int k) { return v << k | v >> (32 - k); }

/* --- XXH32 --------------------------------------------------------------------------------- */

#define P1 0x9E3779B1u
#define P2 0x85EBCA77u
#define P3 0xC2B2AE3Du
#define P4 0x27D4EB2Fu
#define P5 0x165667B1u

static inline cx_u32 round32(cx_u32 v, cx_u32 lane) { return rotl(v + lane * P2, 13) * P1; }

cx_u32 cx_lz4_xxh32(const cx_u8* p, cx_usize n, cx_u32 seed) {
    const cx_u8* end = p + n;
    cx_u32 h;
    if (n >= 16) {
        cx_u32 a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
        const cx_u8* stop = end - 16;
        do {
            a = round32(a, load32(p));
            b = round32(b, load32(p + 4));
            c = round32(c, load32(p + 8));
            d = round32(d, load32(p + 12));
            p += 16;
        } while (p <= stop);
        h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18);
    } else {
        h = seed + P5;
    }
    h += (cx_u32)n;
    for (; end - p >= 4; p += 4) h = rotl(h + load32(p) * P3, 17) * P4;
    for (; p < end; p++) h = rotl(h + *p * P5, 11) * P1;
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

/* lz4-java's seed, and the bits of the hash its streams keep. */
#define SEED 0x9747B28Cu
#define CHECK_MASK 0x0FFFFFFFu

/* --- compressing a block ------------------------------------------------------------------- */

#define BLOCK 65536
#define LEVEL 6 /* log2(BLOCK) - 10 */
#define HEADER 21
#define MIN_MATCH 4
#define LAST_LITERALS 5
#define MF_LIMIT 12
#define HASH_LOG 13

static inline cx_u32 hash4(cx_u32 v) { return (v * 2654435761u) >> (32 - HASH_LOG); }

/* How many bytes from p and q are the same, up to `limit`, with q before p. */
static inline cx_usize same(const cx_u8* p, const cx_u8* q, const cx_u8* limit) {
    const cx_u8* start = p;
    while (limit - p >= 8) {
        cx_u64 x = load64(p) ^ load64(q);
        if (x) return (cx_usize)(p - start) + (cx_usize)(__builtin_ctzll(x) >> 3);
        p += 8, q += 8;
    }
    while (p < limit && *p == *q) p++, q++;
    return (cx_usize)(p - start);
}

static inline cx_u8* put_length(cx_u8* op, cx_usize r) {
    while (r >= 255) *op++ = 255, r -= 255;
    *op++ = (cx_u8)r;
    return op;
}

/* A sequence: `ll` literals from `lit`, which the input has up to `end`, then a match `off` back
 * of `ml` bytes. The output has 16 bytes of room past what's written. */
static inline cx_u8* put_sequence(cx_u8* op, const cx_u8* lit, const cx_u8* end, cx_usize ll, cx_usize off, cx_usize ml) {
    cx_u8* token = op++;
    cx_usize mc = ml - MIN_MATCH;
    *token = (cx_u8)((ll >= 15 ? 15 : ll) << 4 | (mc >= 15 ? 15 : mc));
    if (ll >= 15) op = put_length(op, ll - 15);
    if (ll <= 16 && end - lit >= 16) {
        copy16(op, lit);
    } else {
        memcpy(op, lit, ll);
    }
    op += ll;
    op[0] = (cx_u8)off, op[1] = (cx_u8)(off >> 8);
    op += 2;
    if (mc >= 15) op = put_length(op, mc - 15);
    return op;
}

/* Compresses a block of at most BLOCK bytes, and gives the length. Positions in the table are
 * from the block's start, so they fit 16 bits, and every distance is under 65536. The output has
 * room for the worst case, n + n/255 + 16, and 16 more. */
static cx_usize compress_block(const cx_u8* src, cx_usize n, cx_u8* dst, cx_u16* table) {
    cx_u8* op = dst;
    const cx_u8* anchor = src;
    const cx_u8* end = src + n;
    if (n > MF_LIMIT) {
        const cx_u8* mf_limit = end - MF_LIMIT;
        const cx_u8* match_limit = end - LAST_LITERALS;
        memset(table, 0, sizeof(cx_u16) << HASH_LOG);
        const cx_u8* ip = src + 1;
        for (;;) {
            /* Look for a match, stepping further the longer none turns up. */
            const cx_u8* ref;
            cx_u32 tries = 1 << 6;
            for (;;) {
                if (ip > mf_limit) goto last;
                cx_u32 seq = load32(ip);
                cx_u32 h = hash4(seq);
                ref = src + table[h];
                table[h] = (cx_u16)(ip - src);
                if (ref < ip && load32(ref) == seq) break;
                ip += tries++ >> 6;
            }
            while (ip > anchor && ref > src && ip[-1] == ref[-1]) ip--, ref--;
            for (;;) {
                cx_usize ml = MIN_MATCH + same(ip + MIN_MATCH, ref + MIN_MATCH, match_limit);
                op = put_sequence(op, anchor, end, (cx_usize)(ip - anchor), (cx_usize)(ip - ref), ml);
                ip += ml;
                anchor = ip;
                if (ip > mf_limit) goto last;
                table[hash4(load32(ip - 2))] = (cx_u16)(ip - 2 - src);
                /* A match right after a match goes out with no literals. */
                cx_u32 seq = load32(ip);
                cx_u32 h = hash4(seq);
                ref = src + table[h];
                table[h] = (cx_u16)(ip - src);
                if (!(ref < ip && load32(ref) == seq)) break;
            }
            ip++;
        }
    }
last:;
    cx_usize ll = (cx_usize)(end - anchor);
    *op++ = (cx_u8)((ll >= 15 ? 15 : ll) << 4);
    if (ll >= 15) op = put_length(op, ll - 15);
    memcpy(op, anchor, ll);
    op += ll;
    return (cx_usize)(op - dst);
}

static void put_header(cx_u8* h, int method, cx_u32 clen, cx_u32 len, cx_u32 check) {
    memcpy(h, "LZ4Block", 8);
    h[8] = (cx_u8)(method | LEVEL);
    put32(h + 9, clen);
    put32(h + 13, len);
    put32(h + 17, check);
}

cx_usize cx_lz4_bound(cx_usize n) { return n + (n / BLOCK + 1) * (HEADER + BLOCK / 255 + 16) + HEADER + 16; }

/* Writes the stream for in[0..n) to `out`, which has cx_lz4_bound(n) bytes, and gives its length. */
cx_usize cx_lz4_compress(const cx_u8* in, cx_usize n, cx_u8* out) {
    cx_u16 table[1 << HASH_LOG];
    cx_u8* op = out;
    for (cx_usize at = 0; at < n;) {
        cx_usize k = n - at < BLOCK ? n - at : BLOCK;
        cx_usize c = compress_block(in + at, k, op + HEADER, table);
        int method = 0x20;
        if (c >= k) {
            memcpy(op + HEADER, in + at, k);
            c = k, method = 0x10;
        }
        put_header(op, method, (cx_u32)c, (cx_u32)k, cx_lz4_xxh32(in + at, k, SEED) & CHECK_MASK);
        op += HEADER + c;
        at += k;
    }
    put_header(op, 0x10, 0, 0, 0);
    return (cx_usize)(op + HEADER - out);
}

/* --- decompressing a block ----------------------------------------------------------------- */

/* Copies a match of `len` bytes from `dist` back. With `wild`, it may write up to 32 bytes past
 * the end. A match 0 back is zeros, as lz4-java writes it. */
static inline void copy_match(cx_u8* op, cx_usize dist, cx_usize len, int wild) {
    if (dist == 0) {
        memset(op, 0, len);
        return;
    }
    const cx_u8* from = op - dist;
    if (!wild) {
        for (cx_usize i = 0; i < len; i++) op[i] = from[i];
        return;
    }
    cx_u8* end = op + len;
    if (dist >= 16) {
        do {
            copy16(op, from);
            op += 16, from += 16;
        } while (op < end);
    } else if (dist >= 8) {
        do {
            store64(op, load64(from));
            op += 8, from += 8;
        } while (op < end);
    } else {
        /* The first 8 bytes one at a time, then 8 at a time from a whole number of periods
         * back that's at least 8. */
        static const cx_u8 period[8] = {0, 8, 8, 9, 8, 10, 12, 14};
        for (int i = 0; i < 8; i++) op[i] = from[i];
        cx_usize p = period[dist];
        for (cx_u8* q = op + 8; q < end; q += 8) store64(q, load64(q - p));
    }
}

/* A match's length past 15. */
static inline int match_length(const cx_u8** at, const cx_u8* iend, cx_usize* ml) {
    const cx_u8* ip = *at;
    cx_u32 b;
    do {
        if (ip >= iend) return 0;
        b = *ip++;
        *ml += b;
    } while (b == 255);
    *at = ip;
    return 1;
}

/* Decodes a block of `n` bytes to exactly `m` bytes at `dst`, by the rules of lz4-java's fast
 * decompressor, which LZ4BlockInputStream uses. Literals are the last when they end within 8 bytes
 * of the block's end, or when fewer than 8 bytes would follow them in lz4-java's buffer, which is
 * `avail` bytes, at least `n`; the last must end the block. A match comes from inside the block,
 * or 0 back, which is zeros, and ends inside it. The block uses every byte of the input. 0, or -1
 * for a block that breaks them. */
static int decode_block(const cx_u8* ip, cx_usize n, cx_usize avail, cx_u8* dst, cx_usize m) {
    const cx_u8* base = ip;
    const cx_u8* iend = ip + n;
    cx_u8* op = dst;
    cx_u8* oend = dst + m;
    for (;;) {
        if (ip >= iend) return -1;
        cx_u32 token = *ip++;
        cx_usize ll = token >> 4;
        if (ll < 15 && iend - ip >= 32 && oend - op >= 64) {
            /* The common case, with room for copies that go past the end. */
            copy16(op, ip);
            op += ll, ip += ll;
            cx_usize dist = (cx_usize)ip[0] | (cx_usize)ip[1] << 8;
            ip += 2;
            if (dist > (cx_usize)(op - dst)) return -1;
            cx_usize ml = token & 15;
            if (ml < 15) {
                /* At most 18 bytes, copied in whole pieces with no loop: each piece's source is
                 * written before it's read, since it's at least a piece back. */
                const cx_u8* from = op - dist;
                if (dist >= 16) {
                    copy16(op, from);
                    copy16(op + 16, from + 16);
                } else if (dist >= 8) {
                    store64(op, load64(from));
                    store64(op + 8, load64(from + 8));
                    store64(op + 16, load64(from + 16));
                } else {
                    copy_match(op, dist, ml + MIN_MATCH, 1);
                }
                op += ml + MIN_MATCH;
                continue;
            }
            if (!match_length(&ip, iend, &ml)) return -1;
            ml += MIN_MATCH;
            if (ml > (cx_usize)(oend - op)) return -1;
            copy_match(op, dist, ml, (cx_usize)(oend - op) - ml >= 32);
            op += ml;
            continue;
        }
        if (ll == 15) {
            cx_u32 b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                ll += b;
            } while (b == 255);
        }
        if (ll > (cx_usize)(iend - ip) || ll > (cx_usize)(oend - op)) return -1;
        cx_u8* cpy = op + ll;
        if ((cx_usize)(oend - cpy) < 8 || avail - (cx_usize)(ip - base) < 8 + ll) {
            /* The last sequence: its literals end the input and the block. */
            if (ip + ll != iend || cpy != oend) return -1;
            memcpy(op, ip, ll);
            return 0;
        }
        if ((cx_usize)(iend - (ip + ll)) < 2) return -1;
        memcpy(op, ip, ll);
        ip += ll, op = cpy;
        cx_usize dist = (cx_usize)ip[0] | (cx_usize)ip[1] << 8;
        ip += 2;
        if (dist > (cx_usize)(op - dst)) return -1;
        cx_usize ml = token & 15;
        if (ml == 15 && !match_length(&ip, iend, &ml)) return -1;
        ml += MIN_MATCH;
        if (ml > (cx_usize)(oend - op)) return -1;
        copy_match(op, dist, ml, (cx_usize)(oend - op) - ml >= 32);
        op += ml;
    }
}

/* --- the stream ---------------------------------------------------------------------------- */

static inline cx_u32 get32(const cx_u8* p) {
    return (cx_u32)p[0] | (cx_u32)p[1] << 8 | (cx_u32)p[2] << 16 | (cx_u32)p[3] << 24;
}

/* Decodes one block to `to`, which has `len` bytes, and checks it. */
static cx_i32 block_to(const cx_u8* data, int method, cx_u32 clen, cx_usize avail, cx_u32 len, cx_u32 check, cx_u8* to) {
    if (method == 0x10) {
        memcpy(to, data, len);
    } else if (decode_block(data, clen, avail, to, len) != 0) {
        return -1;
    }
    return (cx_lz4_xxh32(to, len, SEED) & CHECK_MASK) == check ? 0 : -3;
}

/* What the blocks from `at` on decompress to, as far as their headers go. */
static cx_usize rest_len(const cx_u8* in, cx_usize n, cx_usize at) {
    cx_usize total = 0;
    while (n - at >= HEADER && memcmp(in + at, "LZ4Block", 8) == 0) {
        cx_u32 clen = get32(in + at + 9), olen = get32(in + at + 13);
        if (olen == 0 || clen > n - at - HEADER) break;
        total += olen;
        at += HEADER + clen;
    }
    return total;
}

/* Decodes the stream in[0..n) onto out, from where the state says: state[0] is where the next
 * header is, state[1] the output's length, and state[3] the length lz4-java's buffer for
 * compressed blocks would have, which starts at 21 and grows as it grows it, to the block's length
 * or half again its own, whichever is more. Whole blocks go while they fit before `end`. Gives 0 at
 * the empty block that ends the stream; 1 when the next block doesn't fit, with state[2] what the
 * rest decompresses to as its headers say; 2 when it stopped at `end`, which is the last room, with
 * the block that didn't fit cut there; and -1, -2 or -3 for a stream that's corrupt, cut off, or
 * fails a checksum. */
cx_i32 cx_lz4_decode(const cx_u8* in, cx_usize n, cx_usize* state, cx_u8* out, cx_usize end, cx_bool last) {
    for (;;) {
        cx_usize at = state[0], len = state[1];
        if (n - at < HEADER) return -2;
        const cx_u8* h = in + at;
        if (memcmp(h, "LZ4Block", 8) != 0) return -1;
        int method = h[8] & 0xF0, level = h[8] & 0x0F;
        cx_u32 clen = get32(h + 9), olen = get32(h + 13), check = get32(h + 17);
        if (method != 0x10 && method != 0x20) return -1;
        if (olen > (1u << (10 + level)) || clen > 0x7FFFFFFFu || (olen == 0) != (clen == 0) || (method == 0x10 && olen != clen)) {
            return -1;
        }
        if (olen == 0) {
            if (check != 0) return -1;
            state[0] = at + HEADER;
            return 0;
        }
        if (n - at - HEADER < clen) return -2;
        cx_usize avail = state[3];
        if (method == 0x20 && clen > avail) {
            avail = avail * 3 / 2 > clen ? avail * 3 / 2 : clen;
            state[3] = avail;
        }
        if (olen > end - len) {
            if (!last) {
                state[2] = rest_len(in, n, at);
                return 1;
            }
            cx_u8* aside = malloc(olen);
            if (!aside) cx_panic_capacity();
            cx_i32 s = block_to(h + HEADER, method, clen, avail, olen, check, aside);
            if (s == 0) {
                memcpy(out + len, aside, end - len);
                state[1] = end;
            }
            free(aside);
            return s == 0 ? 2 : s;
        }
        cx_i32 s = block_to(h + HEADER, method, clen, avail, olen, check, out + len);
        if (s != 0) return s;
        state[0] = at + HEADER + clen;
        state[1] = len + olen;
    }
}
