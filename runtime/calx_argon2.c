/* argon2d, argon2i and argon2id, version 0x13, behind std.crypto's password hashing (CLX-384).
 * Linked only into programs that call it. Written from RFC 9106; BLAKE2b, which it hashes its
 * first and last blocks with, is in calx_digest.c.
 *
 * The memory is `memory` KiB blocks in `lanes` rows, each row cut into four slices. Each block is
 * the compression G of the block before it and one chosen from those already made, which for
 * argon2d is picked by the block before's first word, for argon2i by a counter run through G, and
 * for argon2id by the counter in the first half of the first pass and the block before after.
 * Within a slice the lanes don't look at each other's blocks of that slice, so they run at once,
 * each on a thread of its own, and meet at the slice's end, as the reference implementation's
 * threads do. G is BLAKE2b's round with a multiply in each add, on the 1024-byte block as eight
 * rows of 16 words and then eight columns. */
#include "calx_rt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

enum { ARGON2D, ARGON2I, ARGON2ID };
enum { SLICES = 4, ADDRESSES = 128, MAX_THREADS = 16 };

typedef struct {
    cx_u64 v[128];
} block;

static inline cx_u64 rotr64(cx_u64 x, int n) { return (x >> n) | (x << (64 - n)); }

/* BLAKE2b's add with the product of the two words' low halves added twice, so each step mixes
 * through a multiply too. */
static inline cx_u64 blamka(cx_u64 x, cx_u64 y) { return x + y + 2 * (cx_u64)(cx_u32)x * (cx_u32)y; }

#define GB(a, b, c, d)          \
    do {                        \
        a = blamka(a, b);       \
        d = rotr64(d ^ a, 32);  \
        c = blamka(c, d);       \
        b = rotr64(b ^ c, 24);  \
        a = blamka(a, b);       \
        d = rotr64(d ^ a, 16);  \
        c = blamka(c, d);       \
        b = rotr64(b ^ c, 63);  \
    } while (0)

#define ROUND(v, i0, i1, i2, i3, i4, i5, i6, i7, i8, i9, i10, i11, i12, i13, i14, i15) \
    do {                                                                                 \
        GB(v[i0], v[i4], v[i8], v[i12]);                                                 \
        GB(v[i1], v[i5], v[i9], v[i13]);                                                 \
        GB(v[i2], v[i6], v[i10], v[i14]);                                                \
        GB(v[i3], v[i7], v[i11], v[i15]);                                                \
        GB(v[i0], v[i5], v[i10], v[i15]);                                                \
        GB(v[i1], v[i6], v[i11], v[i12]);                                                \
        GB(v[i2], v[i7], v[i8], v[i13]);                                                 \
        GB(v[i3], v[i4], v[i9], v[i14]);                                                 \
    } while (0)

/* G: R = prev ^ ref, permuted by rows then columns, and xored with R again. From the second pass
 * on, version 0x13 xors the block it overwrites in too. */
static void fill_block(const block* prev, const block* ref, block* next, int with_xor) {
    block r, keep;
    for (int i = 0; i < 128; i++) r.v[i] = prev->v[i] ^ ref->v[i];
    if (with_xor) {
        for (int i = 0; i < 128; i++) keep.v[i] = r.v[i] ^ next->v[i];
    } else {
        keep = r;
    }
    cx_u64* v = r.v;
    for (int i = 0; i < 128; i += 16) {
        ROUND(v, i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8, i + 9, i + 10, i + 11, i + 12, i + 13, i + 14, i + 15);
    }
    for (int i = 0; i < 16; i += 2) {
        ROUND(v, i, i + 1, i + 16, i + 17, i + 32, i + 33, i + 48, i + 49, i + 64, i + 65, i + 80, i + 81, i + 96, i + 97, i + 112,
              i + 113);
    }
    for (int i = 0; i < 128; i++) next->v[i] = keep.v[i] ^ v[i];
}

static void put32(cx_u8* p, cx_u32 v) {
    p[0] = (cx_u8)v;
    p[1] = (cx_u8)(v >> 8);
    p[2] = (cx_u8)(v >> 16);
    p[3] = (cx_u8)(v >> 24);
}

/* Wipes what held secrets, in a way clang can't leave out as a store nothing reads. */
static void wipe(void* p, cx_usize n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/* H': BLAKE2b of the length and the input for 64 bytes or fewer. For more, a chain of 64-byte
 * hashes, each one's first half taken, and the last of what's left whole. */
static void hash_long(cx_u8* out, cx_usize outlen, const cx_u8* in, cx_usize inlen) {
    cx_u8 ctx[CX_DIGEST_CTX], len[4], v[64];
    put32(len, (cx_u32)outlen);
    cx_digest_blake2b_start(ctx, outlen <= 64 ? outlen : 64, NULL, 0);
    cx_digest_blake2b_add(ctx, len, 4);
    cx_digest_blake2b_add(ctx, in, inlen);
    if (outlen <= 64) {
        cx_digest_blake2b_finish(ctx, out);
        return;
    }
    cx_digest_blake2b_finish(ctx, v);
    memcpy(out, v, 32);
    out += 32;
    cx_usize left = outlen - 32;
    while (left > 64) {
        cx_digest_blake2b_start(ctx, 64, NULL, 0);
        cx_digest_blake2b_add(ctx, v, 64);
        cx_digest_blake2b_finish(ctx, v);
        memcpy(out, v, 32);
        out += 32;
        left -= 32;
    }
    cx_digest_blake2b_start(ctx, left, NULL, 0);
    cx_digest_blake2b_add(ctx, v, 64);
    cx_digest_blake2b_finish(ctx, out);
    wipe(v, sizeof v);
    wipe(ctx, sizeof ctx);
}

typedef struct {
    block* memory;
    cx_u32 type, passes, lanes, lane_length, segment_length, memory_blocks;
} instance;

typedef struct {
    const instance* in;
    cx_u32 pass, slice, first_lane, lane_step;
} work;

/* Where in its lane the block to reference is, from the low half of the pseudo-random word: the
 * blocks it may take are those made so far, less the one just before, and on later passes those
 * of the last three slices; the square of the word, taken from the top, favours the newest. */
static cx_u32 index_alpha(const instance* in, cx_u32 pass, cx_u32 slice, cx_u32 index, cx_u32 rand, int same_lane) {
    cx_u32 area;
    if (pass == 0) {
        if (slice == 0) {
            area = index - 1;
        } else if (same_lane) {
            area = slice * in->segment_length + index - 1;
        } else {
            area = slice * in->segment_length - (index == 0 ? 1 : 0);
        }
    } else if (same_lane) {
        area = in->lane_length - in->segment_length + index - 1;
    } else {
        area = in->lane_length - in->segment_length - (index == 0 ? 1 : 0);
    }
    cx_u64 rel = (cx_u64)rand * rand >> 32;
    rel = area - 1 - ((cx_u64)area * rel >> 32);
    cx_u32 start = pass != 0 && slice != SLICES - 1 ? (slice + 1) * in->segment_length : 0;
    return (cx_u32)((start + rel) % in->lane_length);
}

/* The next 128 pseudo-random words for argon2i's references: G twice over the zero block, of the
 * counter block with its count raised by one. */
static void next_addresses(block* addresses, block* input, const block* zero) {
    input->v[6]++;
    fill_block(zero, input, addresses, 0);
    fill_block(zero, addresses, addresses, 0);
}

static void fill_segment(const instance* in, cx_u32 pass, cx_u32 slice, cx_u32 lane) {
    block addresses, input, zero;
    int independent = in->type == ARGON2I || (in->type == ARGON2ID && pass == 0 && slice < SLICES / 2);
    if (independent) {
        memset(&zero, 0, sizeof zero);
        memset(&input, 0, sizeof input);
        input.v[0] = pass;
        input.v[1] = lane;
        input.v[2] = slice;
        input.v[3] = in->memory_blocks;
        input.v[4] = in->passes;
        input.v[5] = in->type;
    }
    cx_u32 start = 0;
    if (pass == 0 && slice == 0) {
        start = 2;  // the first two blocks of each lane come from the hash of the inputs
        if (independent) next_addresses(&addresses, &input, &zero);
    }
    cx_u32 cur = lane * in->lane_length + slice * in->segment_length + start;
    cx_u32 prev = cur % in->lane_length == 0 ? cur + in->lane_length - 1 : cur - 1;
    for (cx_u32 i = start; i < in->segment_length; i++, cur++, prev++) {
        if (cur % in->lane_length == 1) prev = cur - 1;
        cx_u64 rand;
        if (independent) {
            if (i % ADDRESSES == 0) next_addresses(&addresses, &input, &zero);
            rand = addresses.v[i % ADDRESSES];
        } else {
            rand = in->memory[prev].v[0];
        }
        cx_u32 ref_lane = pass == 0 && slice == 0 ? lane : (cx_u32)((rand >> 32) % in->lanes);
        cx_u32 ref = index_alpha(in, pass, slice, i, (cx_u32)rand, ref_lane == lane);
        fill_block(in->memory + prev, in->memory + (cx_u64)in->lane_length * ref_lane + ref, in->memory + cur, pass != 0);
    }
}

static void run_work(const work* w) {
    for (cx_u32 lane = w->first_lane; lane < w->in->lanes; lane += w->lane_step) fill_segment(w->in, w->pass, w->slice, lane);
}

#ifdef _WIN32
static DWORD WINAPI work_main(LPVOID p) {
    run_work((const work*)p);
    return 0;
}
#else
static void* work_main(void* p) {
    run_work((const work*)p);
    return NULL;
}
#endif

/* One slice of every lane: lanes split among up to MAX_THREADS threads, this one among them. A
 * thread that can't be started has its lanes run here instead, which gives the same blocks. */
static void fill_slice(const instance* in, cx_u32 pass, cx_u32 slice) {
    cx_u32 threads = in->lanes < MAX_THREADS ? in->lanes : MAX_THREADS;
    work w[MAX_THREADS];
    int started[MAX_THREADS] = {0};
#ifdef _WIN32
    HANDLE handles[MAX_THREADS];
#else
    pthread_t handles[MAX_THREADS];
#endif
    for (cx_u32 k = 0; k < threads; k++) w[k] = (work){in, pass, slice, k, threads};
    for (cx_u32 k = 1; k < threads; k++) {
#ifdef _WIN32
        handles[k] = CreateThread(NULL, 256 * 1024, work_main, &w[k], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        started[k] = handles[k] != NULL;
#else
        started[k] = pthread_create(&handles[k], NULL, work_main, &w[k]) == 0;
#endif
    }
    run_work(&w[0]);
    for (cx_u32 k = 1; k < threads; k++) {
        if (!started[k]) {
            run_work(&w[k]);
            continue;
        }
#ifdef _WIN32
        WaitForSingleObject(handles[k], INFINITE);
        CloseHandle(handles[k]);
#else
        pthread_join(handles[k], NULL);
#endif
    }
}

cx_bool cx_argon2_hash(cx_u32 type, cx_u32 memory, cx_u32 passes, cx_u32 lanes, const cx_u8* pwd, cx_usize pwdlen,
                       const cx_u8* salt, cx_usize saltlen, const cx_u8* secret, cx_usize secretlen, const cx_u8* ad,
                       cx_usize adlen, cx_u8* out, cx_usize outlen) {
    if (type > ARGON2ID || passes < 1 || lanes < 1 || lanes > 0xFFFFFF || outlen < 4 || (cx_u64)memory < 8 * (cx_u64)lanes) return 0;
    if (pwdlen > 0xFFFFFFFFu || saltlen > 0xFFFFFFFFu || secretlen > 0xFFFFFFFFu || adlen > 0xFFFFFFFFu || outlen > 0xFFFFFFFFu) return 0;
    instance in;
    in.type = type;
    in.passes = passes;
    in.lanes = lanes;
    in.segment_length = memory / (lanes * SLICES);
    in.lane_length = in.segment_length * SLICES;
    in.memory_blocks = in.lane_length * lanes;
    if ((cx_u64)in.memory_blocks * sizeof(block) > (cx_u64)(cx_usize)-1) return 0;
    in.memory = (block*)malloc((cx_usize)in.memory_blocks * sizeof(block));
    if (!in.memory) return 0;

    // H0, the hash of every parameter and input, which each lane's first two blocks start from.
    cx_u8 ctx[CX_DIGEST_CTX], word[4], h0[72];
    cx_digest_blake2b_start(ctx, 64, NULL, 0);
    cx_u32 head[6] = {lanes, (cx_u32)outlen, memory, passes, 0x13, type};
    for (int i = 0; i < 6; i++) {
        put32(word, head[i]);
        cx_digest_blake2b_add(ctx, word, 4);
    }
    const cx_u8* parts[4] = {pwd, salt, secret, ad};
    cx_usize lens[4] = {pwdlen, saltlen, secretlen, adlen};
    for (int i = 0; i < 4; i++) {
        put32(word, (cx_u32)lens[i]);
        cx_digest_blake2b_add(ctx, word, 4);
        if (lens[i]) cx_digest_blake2b_add(ctx, parts[i], lens[i]);
    }
    cx_digest_blake2b_finish(ctx, h0);
    wipe(ctx, sizeof ctx);
    for (cx_u32 l = 0; l < lanes; l++) {
        for (cx_u32 j = 0; j < 2; j++) {
            put32(h0 + 64, j);
            put32(h0 + 68, l);
            hash_long((cx_u8*)in.memory[(cx_u64)l * in.lane_length + j].v, 1024, h0, 72);
        }
    }
    wipe(h0, sizeof h0);

    for (cx_u32 pass = 0; pass < passes; pass++) {
        for (cx_u32 slice = 0; slice < SLICES; slice++) fill_slice(&in, pass, slice);
    }

    // The tag: H' of the xor of each lane's last block.
    block last = in.memory[in.lane_length - 1];
    for (cx_u32 l = 1; l < lanes; l++) {
        const block* b = in.memory + (cx_u64)l * in.lane_length + in.lane_length - 1;
        for (int i = 0; i < 128; i++) last.v[i] ^= b->v[i];
    }
    hash_long(out, outlen, (const cx_u8*)last.v, sizeof last);
    wipe(&last, sizeof last);
    wipe(in.memory, (cx_usize)in.memory_blocks * sizeof(block));
    free(in.memory);
    return 1;
}
