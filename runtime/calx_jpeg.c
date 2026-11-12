/* The pixel work and Huffman coding of std.image.jpeg's encoder (CLX-148): RGB to YCbCr, the
 * chroma averaged down, each block through the forward DCT and quantized, and the blocks coded.
 * Linked only into programs that write JPEGs. The Calx side lays out the rows, walks the MCUs and
 * writes the file's headers.
 *
 * The arithmetic is libjpeg's, so the bytes are Pillow's: jccolor.c's 16-bit fixed-point color
 * conversion, jcsample.c's averaging with its alternating rounding, and jfdctint.c's accurate
 * integer DCT ("islow", 13-bit constants, two passes), each coefficient then divided by its
 * quantizer times 8, rounding a half away from zero. Written from those formulas; the AVX2 code is
 * our own arrangement of them, and gives the same integers as the plain C beside it, which builds
 * without AVX2 use. On Arm64 NEON code does the same work in the same integers (CLX-281).
 *
 * The DCT works on 16-bit lanes, two blocks at once, one in each half of a 256-bit register, so a
 * row of a block is 8 lanes. Each multiply-and-add is pmaddwd on a pair of values, so each output
 * is the same sum of products libjpeg's C makes, exactly, in 32 bits. Every value that's added in
 * 16 bits fits. Each is linear in the samples, -128 to 127, so it's at most 128 times the sum of
 * its weights' sizes, plus the rounding of pass 1's outputs, under 1 each: pass 1's values are
 * under 4,097 in size, pass 2's sums of two or four of them under 16,393, and its t10 + t11 and
 * t10 - t11 from -32,768 (a block of -128s) to 32,640. Pass 2's outputs are under 8,193.
 *
 * The quantizer divides a = |t| + d / 2, under 2^14, by d = 8 to 2,040: it takes the high 16 bits
 * of a times r = ceil(2^k / d), with k = 14 + d's bit length, and then of that times 2^(32 - k).
 * That's floor(a r / 2^k), which is floor(a / d) since a (r d - 2^k) < 2^14 d < 2^k; all 255
 * divisors and 2^14 dividends were tried. */
#include "calx_rt.h"

#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define CX_NEON 1
#endif

/* Where each coefficient, in zigzag order, is in a block, row by row. */
static const cx_u8 NATURAL[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* For each byte of a block's nonzero mask in row order, its bits in zigzag order: ZZ[i][v] has bit
 * k for each bit j of v where the zigzag's k-th coefficient is the (8i + j)-th. Made once. */
static cx_u64 ZZ[8][256];
static int zz_ready;

static void zz_init(void) {
    if (__atomic_load_n(&zz_ready, __ATOMIC_ACQUIRE)) return;
    cx_u8 at[64];
    for (int k = 0; k < 64; k++) at[NATURAL[k]] = (cx_u8)k;
    for (int i = 0; i < 8; i++) {
        for (int v = 0; v < 256; v++) {
            cx_u64 m = 0;
            for (int j = 0; j < 8; j++) {
                if (v >> j & 1) m |= 1ull << at[8 * i + j];
            }
            ZZ[i][v] = m;
        }
    }
    __atomic_store_n(&zz_ready, 1, __ATOMIC_RELEASE);
}

/* The mask in zigzag order, without the DC's bit, of a block whose nonzero coefficients in row
 * order are the bits of nz. */
static inline cx_u64 zigzag_mask(cx_u64 nz) {
    cx_u64 m = 0;
    for (int i = 0; i < 8; i++) m |= ZZ[i][(nz >> (8 * i)) & 0xFF];
    return m & ~1ull;
}

void cx_jpg_divisors(const cx_u16* table, cx_u16* q) {
    zz_init();
    for (int i = 0; i < 64; i++) {
        const cx_u32 d = (cx_u32)table[i] << 3;
        const cx_u32 bits = 32 - (cx_u32)__builtin_clz(d);
        const cx_u32 k = 14 + bits;
        q[i] = (cx_u16)(((1u << k) + d - 1) / d);
        q[64 + i] = (cx_u16)(1u << (16 - (k - 16)));
        q[128 + i] = (cx_u16)(d >> 1);
    }
}

/* --- scalar ---------------------------------------------------------------------------------- */

/* libjpeg's constants, FIX(x) = x * 2^13 rounded. */
enum {
    K0298 = 2446,
    K0390 = 3196,
    K0541 = 4433,
    K0765 = 6270,
    K0899 = 7373,
    K1175 = 9633,
    K1501 = 12299,
    K1847 = 15137,
    K1961 = 16069,
    K2053 = 16819,
    K2562 = 20995,
    K3072 = 25172,
};

/* One pass of the DCT over x[0], x[step], ..., x[7 step], in place: pass 1 scales by 4 and
 * descales the products by 11 bits, pass 2 descales the sums by 2 and the products by 15. */
static void dct8(cx_i32* x, int step, int pass) {
    const cx_i32 x0 = x[0], x1 = x[step], x2 = x[2 * step], x3 = x[3 * step];
    const cx_i32 x4 = x[4 * step], x5 = x[5 * step], x6 = x[6 * step], x7 = x[7 * step];
    const cx_i32 t0 = x0 + x7, t7 = x0 - x7, t1 = x1 + x6, t6 = x1 - x6;
    const cx_i32 t2 = x2 + x5, t5 = x2 - x5, t3 = x3 + x4, t4 = x3 - x4;
    const cx_i32 t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    const int n = pass == 1 ? 11 : 15;
    const cx_i32 r = 1 << (n - 1);
    if (pass == 1) {
        x[0] = (t10 + t11) * 4;
        x[4 * step] = (t10 - t11) * 4;
    } else {
        x[0] = (t10 + t11 + 2) >> 2;
        x[4 * step] = (t10 - t11 + 2) >> 2;
    }
    const cx_i32 z1 = (t12 + t13) * K0541;
    x[2 * step] = (z1 + t13 * K0765 + r) >> n;
    x[6 * step] = (z1 - t12 * K1847 + r) >> n;
    const cx_i32 z5 = (t4 + t5 + t6 + t7) * K1175;
    const cx_i32 w1 = (t4 + t7) * -K0899, w2 = (t5 + t6) * -K2562;
    const cx_i32 w3 = (t4 + t6) * -K1961 + z5, w4 = (t5 + t7) * -K0390 + z5;
    x[7 * step] = (t4 * K0298 + w1 + w3 + r) >> n;
    x[5 * step] = (t5 * K2053 + w2 + w4 + r) >> n;
    x[3 * step] = (t6 * K3072 + w2 + w3 + r) >> n;
    x[step] = (t7 * K1501 + w1 + w4 + r) >> n;
}

static cx_u64 fdct_scalar(const cx_u8* src, cx_usize stride, const cx_u16* q, cx_i16* out) {
    cx_i32 w[64];
    for (int r = 0; r < 8; r++) {
        for (int j = 0; j < 8; j++) w[8 * r + j] = (cx_i32)src[r * stride + j] - 128;
        dct8(w + 8 * r, 1, 1);
    }
    for (int c = 0; c < 8; c++) dct8(w + c, 8, 2);
    cx_u64 nz = 0;
    for (int i = 0; i < 64; i++) {
        const cx_i32 t = w[i];
        const cx_i32 d = (cx_i32)q[128 + i] * 2;
        const cx_i32 v = ((t < 0 ? -t : t) + (d >> 1)) / d;
        out[i] = (cx_i16)(t < 0 ? -v : v);
        if (v) nz |= 1ull << i;
    }
    return zigzag_mask(nz);
}

/* --- AVX2 ------------------------------------------------------------------------------------ */

#if defined(__AVX2__)
typedef __m256i V;

/* A pmaddwd constant: each 32-bit lane's low half is a's factor and its high half b's. */
#define PAIR(a, b) _mm256_set1_epi32((cx_i32)(((cx_u32)(cx_u16)(cx_i16)(a)) | ((cx_u32)(cx_u16)(cx_i16)(b) << 16)))

/* Each half's 8 rows of 8 lanes turned into its 8 columns. */
static inline __attribute__((always_inline)) void transpose(V* r) {
    const V a0 = _mm256_unpacklo_epi16(r[0], r[1]), a1 = _mm256_unpackhi_epi16(r[0], r[1]);
    const V a2 = _mm256_unpacklo_epi16(r[2], r[3]), a3 = _mm256_unpackhi_epi16(r[2], r[3]);
    const V a4 = _mm256_unpacklo_epi16(r[4], r[5]), a5 = _mm256_unpackhi_epi16(r[4], r[5]);
    const V a6 = _mm256_unpacklo_epi16(r[6], r[7]), a7 = _mm256_unpackhi_epi16(r[6], r[7]);
    const V b0 = _mm256_unpacklo_epi32(a0, a2), b1 = _mm256_unpackhi_epi32(a0, a2);
    const V b2 = _mm256_unpacklo_epi32(a1, a3), b3 = _mm256_unpackhi_epi32(a1, a3);
    const V b4 = _mm256_unpacklo_epi32(a4, a6), b5 = _mm256_unpackhi_epi32(a4, a6);
    const V b6 = _mm256_unpacklo_epi32(a5, a7), b7 = _mm256_unpackhi_epi32(a5, a7);
    r[0] = _mm256_unpacklo_epi64(b0, b4);
    r[1] = _mm256_unpackhi_epi64(b0, b4);
    r[2] = _mm256_unpacklo_epi64(b1, b5);
    r[3] = _mm256_unpackhi_epi64(b1, b5);
    r[4] = _mm256_unpacklo_epi64(b2, b6);
    r[5] = _mm256_unpackhi_epi64(b2, b6);
    r[6] = _mm256_unpacklo_epi64(b3, b7);
    r[7] = _mm256_unpackhi_epi64(b3, b7);
}

/* The 32-bit sums lo (lanes 0 to 3 of each half) and hi (4 to 7) rounded, shifted down n bits and
 * put back in 16. */
#define DESCALE(lo, hi, n)                                                                        \
    _mm256_packs_epi32(_mm256_srai_epi32(_mm256_add_epi32((lo), _mm256_set1_epi32(1 << ((n) - 1))), (n)), \
                       _mm256_srai_epi32(_mm256_add_epi32((hi), _mm256_set1_epi32(1 << ((n) - 1))), (n)))

/* One pass of the DCT across the 8 registers, each lane a row (pass 1) or a column (pass 2), as
 * dct8. Each product sum is regrouped so that a pair of values goes into one pmaddwd: z1's, for
 * instance, is folded into the factors of t12 and t13, and z5's into those of t4 + t6 and t5 + t7. */
static inline __attribute__((always_inline)) void dct_pass(V* x, int pass) {
    const V t0 = _mm256_add_epi16(x[0], x[7]), t7 = _mm256_sub_epi16(x[0], x[7]);
    const V t1 = _mm256_add_epi16(x[1], x[6]), t6 = _mm256_sub_epi16(x[1], x[6]);
    const V t2 = _mm256_add_epi16(x[2], x[5]), t5 = _mm256_sub_epi16(x[2], x[5]);
    const V t3 = _mm256_add_epi16(x[3], x[4]), t4 = _mm256_sub_epi16(x[3], x[4]);
    const V t10 = _mm256_add_epi16(t0, t3), t13 = _mm256_sub_epi16(t0, t3);
    const V t11 = _mm256_add_epi16(t1, t2), t12 = _mm256_sub_epi16(t1, t2);
    if (pass == 1) {
        x[0] = _mm256_slli_epi16(_mm256_add_epi16(t10, t11), 2);
        x[4] = _mm256_slli_epi16(_mm256_sub_epi16(t10, t11), 2);
    } else {
        const V two = _mm256_set1_epi16(2);
        x[0] = _mm256_srai_epi16(_mm256_add_epi16(_mm256_add_epi16(t10, t11), two), 2);
        x[4] = _mm256_srai_epi16(_mm256_add_epi16(_mm256_sub_epi16(t10, t11), two), 2);
    }
#define PASS_OUT(lo, hi) (pass == 1 ? DESCALE(lo, hi, 11) : DESCALE(lo, hi, 15))
    V l = _mm256_unpacklo_epi16(t12, t13), h = _mm256_unpackhi_epi16(t12, t13);
    x[2] = PASS_OUT(_mm256_madd_epi16(l, PAIR(K0541, K0541 + K0765)), _mm256_madd_epi16(h, PAIR(K0541, K0541 + K0765)));
    x[6] = PASS_OUT(_mm256_madd_epi16(l, PAIR(K0541 - K1847, K0541)), _mm256_madd_epi16(h, PAIR(K0541 - K1847, K0541)));
    const V z3 = _mm256_add_epi16(t4, t6), z4 = _mm256_add_epi16(t5, t7);
    l = _mm256_unpacklo_epi16(z3, z4);
    h = _mm256_unpackhi_epi16(z3, z4);
    /* w3 = z3 (1.175 - 1.961) + z4 1.175 and w4 = z3 1.175 + z4 (1.175 - 0.390). */
    const V w3l = _mm256_madd_epi16(l, PAIR(K1175 - K1961, K1175)), w3h = _mm256_madd_epi16(h, PAIR(K1175 - K1961, K1175));
    const V w4l = _mm256_madd_epi16(l, PAIR(K1175, K1175 - K0390)), w4h = _mm256_madd_epi16(h, PAIR(K1175, K1175 - K0390));
    l = _mm256_unpacklo_epi16(t4, t7);
    h = _mm256_unpackhi_epi16(t4, t7);
    x[7] = PASS_OUT(_mm256_add_epi32(_mm256_madd_epi16(l, PAIR(K0298 - K0899, -K0899)), w3l),
                    _mm256_add_epi32(_mm256_madd_epi16(h, PAIR(K0298 - K0899, -K0899)), w3h));
    x[1] = PASS_OUT(_mm256_add_epi32(_mm256_madd_epi16(l, PAIR(-K0899, K1501 - K0899)), w4l),
                    _mm256_add_epi32(_mm256_madd_epi16(h, PAIR(-K0899, K1501 - K0899)), w4h));
    l = _mm256_unpacklo_epi16(t5, t6);
    h = _mm256_unpackhi_epi16(t5, t6);
    x[5] = PASS_OUT(_mm256_add_epi32(_mm256_madd_epi16(l, PAIR(K2053 - K2562, -K2562)), w4l),
                    _mm256_add_epi32(_mm256_madd_epi16(h, PAIR(K2053 - K2562, -K2562)), w4h));
    x[3] = PASS_OUT(_mm256_add_epi32(_mm256_madd_epi16(l, PAIR(-K2562, K3072 - K2562)), w3l),
                    _mm256_add_epi32(_mm256_madd_epi16(h, PAIR(-K2562, K3072 - K2562)), w3h));
#undef PASS_OUT
}

static inline V halves(const void* a, const void* b) {
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)a)), _mm_loadu_si128((const __m128i*)b), 1);
}
#endif

/* --- NEON ------------------------------------------------------------------------------------ */

#ifdef CX_NEON
/* CLX-281: the same DCT on Arm64, one block at a time, a row in each of 8 registers of 8 16-bit
 * lanes. Each product sum is the AVX2 code's pair, as a widening multiply and a multiply-add on
 * each half of the lanes, and sqrshrn rounds, shifts, narrows and saturates as DESCALE and packs
 * do, so the integers are the same. */
typedef struct {
    int32x4_t lo, hi;
} wide_t;

static inline __attribute__((always_inline)) wide_t pair_n(int16x8_t a, cx_i16 ka, int16x8_t b, cx_i16 kb) {
    wide_t w;
    w.lo = vmlal_n_s16(vmull_n_s16(vget_low_s16(a), ka), vget_low_s16(b), kb);
    w.hi = vmlal_high_n_s16(vmull_high_n_s16(a, ka), b, kb);
    return w;
}

static inline __attribute__((always_inline)) wide_t wide_add(wide_t a, wide_t b) {
    a.lo = vaddq_s32(a.lo, b.lo);
    a.hi = vaddq_s32(a.hi, b.hi);
    return a;
}

#define NARROW(w, n) vqrshrn_high_n_s32(vqrshrn_n_s32((w).lo, n), (w).hi, n)

/* 8 rows of 8 lanes turned into 8 columns: lanes swapped in pairs, then pairs of lanes, then
 * halves. */
static inline __attribute__((always_inline)) void transpose_neon(int16x8_t* r) {
    int16x8_t b[8];
    for (int i = 0; i < 8; i += 2) {
        b[i] = vtrn1q_s16(r[i], r[i + 1]);
        b[i + 1] = vtrn2q_s16(r[i], r[i + 1]);
    }
    int32x4_t c[8];
    for (int i = 0; i < 8; i += 4) {
        c[i] = vtrn1q_s32(vreinterpretq_s32_s16(b[i]), vreinterpretq_s32_s16(b[i + 2]));
        c[i + 2] = vtrn2q_s32(vreinterpretq_s32_s16(b[i]), vreinterpretq_s32_s16(b[i + 2]));
        c[i + 1] = vtrn1q_s32(vreinterpretq_s32_s16(b[i + 1]), vreinterpretq_s32_s16(b[i + 3]));
        c[i + 3] = vtrn2q_s32(vreinterpretq_s32_s16(b[i + 1]), vreinterpretq_s32_s16(b[i + 3]));
    }
    for (int i = 0; i < 4; i++) {
        r[i] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(c[i]), vreinterpretq_s64_s32(c[i + 4])));
        r[i + 4] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(c[i]), vreinterpretq_s64_s32(c[i + 4])));
    }
}

static inline __attribute__((always_inline)) void dct_pass_neon(int16x8_t* x, int pass) {
    const int16x8_t t0 = vaddq_s16(x[0], x[7]), t7 = vsubq_s16(x[0], x[7]);
    const int16x8_t t1 = vaddq_s16(x[1], x[6]), t6 = vsubq_s16(x[1], x[6]);
    const int16x8_t t2 = vaddq_s16(x[2], x[5]), t5 = vsubq_s16(x[2], x[5]);
    const int16x8_t t3 = vaddq_s16(x[3], x[4]), t4 = vsubq_s16(x[3], x[4]);
    const int16x8_t t10 = vaddq_s16(t0, t3), t13 = vsubq_s16(t0, t3);
    const int16x8_t t11 = vaddq_s16(t1, t2), t12 = vsubq_s16(t1, t2);
    if (pass == 1) {
        x[0] = vshlq_n_s16(vaddq_s16(t10, t11), 2);
        x[4] = vshlq_n_s16(vsubq_s16(t10, t11), 2);
    } else {
        x[0] = vrshrq_n_s16(vaddq_s16(t10, t11), 2);
        x[4] = vrshrq_n_s16(vsubq_s16(t10, t11), 2);
    }
#define PASS_OUT(w) (pass == 1 ? NARROW(w, 11) : NARROW(w, 15))
    x[2] = PASS_OUT(pair_n(t12, K0541, t13, K0541 + K0765));
    x[6] = PASS_OUT(pair_n(t12, K0541 - K1847, t13, K0541));
    const int16x8_t z3 = vaddq_s16(t4, t6), z4 = vaddq_s16(t5, t7);
    const wide_t w3 = pair_n(z3, K1175 - K1961, z4, K1175), w4 = pair_n(z3, K1175, z4, K1175 - K0390);
    x[7] = PASS_OUT(wide_add(pair_n(t4, K0298 - K0899, t7, -K0899), w3));
    x[1] = PASS_OUT(wide_add(pair_n(t4, -K0899, t7, K1501 - K0899), w4));
    x[5] = PASS_OUT(wide_add(pair_n(t5, K2053 - K2562, t6, -K2562), w4));
    x[3] = PASS_OUT(wide_add(pair_n(t5, -K2562, t6, K3072 - K2562), w3));
#undef PASS_OUT
}

/* The high 16 bits of each product, as pmulhuw gives. */
static inline uint16x8_t mulhi_u16(uint16x8_t a, uint16x8_t b) {
    return vuzp2q_u16(vreinterpretq_u16_u32(vmull_u16(vget_low_u16(a), vget_low_u16(b))), vreinterpretq_u16_u32(vmull_high_u16(a, b)));
}

/* One block, quantized as the AVX2 code does it, and its zigzag mask. The rows' zero lanes keep
 * their bits of 1 to 128 and pairwise adds gather them, a row to a byte. */
static cx_u64 fdct_neon(const cx_u8* src, cx_usize stride, const cx_u16* q, cx_i16* out) {
    int16x8_t x[8];
    for (int r = 0; r < 8; r++) x[r] = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(vld1_u8(src + r * stride)), vdupq_n_u16(128)));
    transpose_neon(x);
    dct_pass_neon(x, 1);
    transpose_neon(x);
    dct_pass_neon(x, 2);
    uint8x8_t zero[8];
    for (int r = 0; r < 8; r++) {
        const int16x8_t t = x[r];
        const int16x8_t m = vshrq_n_s16(t, 15);
        const uint16x8_t num = vaddq_u16(vreinterpretq_u16_s16(vabsq_s16(t)), vld1q_u16(q + 128 + 8 * r));
        const uint16x8_t v = mulhi_u16(mulhi_u16(num, vld1q_u16(q + 8 * r)), vld1q_u16(q + 64 + 8 * r));
        const int16x8_t s = vsubq_s16(veorq_s16(vreinterpretq_s16_u16(v), m), m);
        vst1q_s16(out + 8 * r, s);
        zero[r] = vmovn_u16(vceqq_s16(s, vdupq_n_s16(0)));
    }
    const uint8x16_t bits = vreinterpretq_u8_u64(vdupq_n_u64(0x8040201008040201ull));
    uint8x16_t p[4];
    for (int i = 0; i < 4; i++) p[i] = vandq_u8(vcombine_u8(zero[2 * i], zero[2 * i + 1]), bits);
    uint8x16_t g = vpaddq_u8(vpaddq_u8(p[0], p[1]), vpaddq_u8(p[2], p[3]));
    return zigzag_mask(~vgetq_lane_u64(vreinterpretq_u64_u8(vpaddq_u8(g, g)), 0));
}
#endif

/* The blocks at a and, if `two`, at b, both rows `stride` bytes apart and divided by q. */
static inline __attribute__((always_inline)) void fdct(const cx_u8* a, const cx_u8* b, cx_usize stride, const cx_u16* q, cx_i16* oa, cx_i16* ob, cx_u64* masks, int two) {
#if defined(__AVX2__)
    V x[8];
    const V center = _mm256_set1_epi16(128);
    for (int r = 0; r < 8; r++) {
        const __m128i rows = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i*)(a + r * stride)), _mm_loadl_epi64((const __m128i*)(b + r * stride)));
        x[r] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(rows), center);
    }
    transpose(x);
    dct_pass(x, 1);
    transpose(x);
    dct_pass(x, 2);
    V z[8];
    for (int r = 0; r < 8; r++) {
        const V t = x[r];
        const V m = _mm256_srai_epi16(t, 15);
        const V abs = _mm256_sub_epi16(_mm256_xor_si256(t, m), m);
        const V num = _mm256_add_epi16(abs, _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(q + 128 + 8 * r))));
        const V recip = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(q + 8 * r)));
        const V scale = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(q + 64 + 8 * r)));
        const V v = _mm256_sub_epi16(_mm256_xor_si256(_mm256_mulhi_epu16(_mm256_mulhi_epu16(num, recip), scale), m), m);
        _mm_storeu_si128((__m128i*)(oa + 8 * r), _mm256_castsi256_si128(v));
        if (two) _mm_storeu_si128((__m128i*)(ob + 8 * r), _mm256_extracti128_si256(v, 1));
        z[r] = _mm256_cmpeq_epi16(v, _mm256_setzero_si256());
    }
    cx_u64 za = 0, zb = 0;
    for (int i = 0; i < 4; i++) {
        const cx_u32 zero = (cx_u32)_mm256_movemask_epi8(_mm256_packs_epi16(z[2 * i], z[2 * i + 1]));
        za |= (cx_u64)(zero & 0xFFFF) << (16 * i);
        zb |= (cx_u64)(zero >> 16) << (16 * i);
    }
    masks[0] = zigzag_mask(~za);
    if (two) masks[1] = zigzag_mask(~zb);
#elif defined(CX_NEON)
    masks[0] = fdct_neon(a, stride, q, oa);
    if (two) masks[1] = fdct_neon(b, stride, q, ob);
#else
    masks[0] = fdct_scalar(a, stride, q, oa);
    if (two) masks[1] = fdct_scalar(b, stride, q, ob);
#endif
}

void cx_jpg_fdct2(const cx_u8* a, const cx_u8* b, cx_usize stride, const cx_u16* q, cx_i16* oa, cx_i16* ob, cx_u64* masks) {
    fdct(a, b, stride, q, oa, ob, masks, 1);
}

cx_u64 cx_jpg_fdct1(const cx_u8* a, cx_usize stride, const cx_u16* q, cx_i16* oa) {
    cx_u64 m;
    fdct(a, a, stride, q, oa, oa, &m, 0);
    return m;
}

/* --- color ----------------------------------------------------------------------------------- */

#if defined(__AVX2__)
/* Eight pixels' Y, Cb and Cr, each in a 32-bit lane, from 16 bytes at p and 16 at p + 12: four
 * pixels in each half. A pshufb makes each pixel's (R, G) a pair of 16-bit lanes and another its
 * (B, G), and every product sum is a pmaddwd on one of the two: 38470 G is 22086 G + 16384 G, and
 * 32768 R or B is twice 16384 of it, so each factor fits in 16 bits. */
static inline __attribute__((always_inline)) void ycc8(const cx_u8* p, V* y, V* cb, V* cr) {
    const V rgm = _mm256_setr_epi8(0, -128, 1, -128, 3, -128, 4, -128, 6, -128, 7, -128, 9, -128, 10, -128,
                                   0, -128, 1, -128, 3, -128, 4, -128, 6, -128, 7, -128, 9, -128, 10, -128);
    const V bgm = _mm256_setr_epi8(2, -128, 1, -128, 5, -128, 4, -128, 8, -128, 7, -128, 11, -128, 10, -128,
                                   2, -128, 1, -128, 5, -128, 4, -128, 8, -128, 7, -128, 11, -128, 10, -128);
    const V s = halves(p, p + 12);
    const V rg = _mm256_shuffle_epi8(s, rgm), bg = _mm256_shuffle_epi8(s, bgm);
    const V half = _mm256_set1_epi32(32768), mid = _mm256_set1_epi32(8421375);
    *y = _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(_mm256_madd_epi16(rg, PAIR(19595, 22086)), _mm256_madd_epi16(bg, PAIR(7471, 16384))), half), 16);
    *cb = _mm256_srli_epi32(
        _mm256_add_epi32(_mm256_add_epi32(_mm256_madd_epi16(rg, PAIR(-11059, -21709)), _mm256_slli_epi32(_mm256_madd_epi16(bg, PAIR(16384, 0)), 1)), mid), 16);
    *cr = _mm256_srli_epi32(
        _mm256_add_epi32(_mm256_add_epi32(_mm256_slli_epi32(_mm256_madd_epi16(rg, PAIR(16384, 0)), 1), _mm256_madd_epi16(bg, PAIR(-5329, -27439))), mid), 16);
}

/* Four ycc8's 32-bit lanes as 32 bytes in pixel order: the packs leave each half's four pixels of
 * a group side by side, which the permute puts back. */
static inline __attribute__((always_inline)) V bytes32(V a, V b, V c, V d) {
    const V v = _mm256_packus_epi16(_mm256_packus_epi32(a, b), _mm256_packus_epi32(c, d));
    return _mm256_permutevar8x32_epi32(v, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
}
#elif defined(CX_NEON)
/* Eight pixels' Y, Cb or Cr: `start` plus each of R, G and B times its factor, added where `add`'s
 * bit for it is set and taken off where not. The sums stay in 32 bits unsigned, as none is ever
 * below 0 once the first factor is in: libjpeg's offsets see to that. */
static inline __attribute__((always_inline)) uint8x8_t ycc_one(uint16x8_t first, cx_u16 k1, uint16x8_t second, cx_u16 k2, int add2,
                                                             uint16x8_t third, cx_u16 k3, int add3, cx_u32 start) {
    uint32x4_t lo = vmlal_n_u16(vdupq_n_u32(start), vget_low_u16(first), k1);
    uint32x4_t hi = vmlal_high_n_u16(vdupq_n_u32(start), first, k1);
    lo = add2 ? vmlal_n_u16(lo, vget_low_u16(second), k2) : vmlsl_n_u16(lo, vget_low_u16(second), k2);
    hi = add2 ? vmlal_high_n_u16(hi, second, k2) : vmlsl_high_n_u16(hi, second, k2);
    lo = add3 ? vmlal_n_u16(lo, vget_low_u16(third), k3) : vmlsl_n_u16(lo, vget_low_u16(third), k3);
    hi = add3 ? vmlal_high_n_u16(hi, third, k3) : vmlsl_high_n_u16(hi, third, k3);
    return vmovn_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi, 16)));
}

static inline __attribute__((always_inline)) void ycc8_neon(uint8x8_t r8, uint8x8_t g8, uint8x8_t b8, cx_u8* y, cx_u8* cb, cx_u8* cr) {
    const uint16x8_t r = vmovl_u8(r8), g = vmovl_u8(g8), b = vmovl_u8(b8);
    vst1_u8(y, ycc_one(r, 19595, g, 38470, 1, b, 7471, 1, 32768));
    vst1_u8(cb, ycc_one(b, 32768, r, 11059, 0, g, 21709, 0, 8421375));
    vst1_u8(cr, ycc_one(r, 32768, g, 27439, 0, b, 5329, 0, 8421375));
}
#endif

void cx_jpg_ycc(const cx_u8* rgb, cx_usize n, cx_u8* y, cx_u8* cb, cx_u8* cr) {
    cx_usize x = 0;
#if defined(__AVX2__)
    /* A group's 16 bytes at p + 12 reach 4 past its pixels, so the last group stops 2 short. */
    for (; x + 34 <= n; x += 32) {
        V Y[4], B[4], R[4];
        for (int g = 0; g < 4; g++) ycc8(rgb + 3 * (x + 8 * g), &Y[g], &B[g], &R[g]);
        _mm256_storeu_si256((V*)(y + x), bytes32(Y[0], Y[1], Y[2], Y[3]));
        _mm256_storeu_si256((V*)(cb + x), bytes32(B[0], B[1], B[2], B[3]));
        _mm256_storeu_si256((V*)(cr + x), bytes32(R[0], R[1], R[2], R[3]));
    }
    for (; x + 10 <= n; x += 8) {
        V Y, B, R;
        ycc8(rgb + 3 * x, &Y, &B, &R);
        _mm_storel_epi64((__m128i*)(y + x), _mm256_castsi256_si128(bytes32(Y, Y, Y, Y)));
        _mm_storel_epi64((__m128i*)(cb + x), _mm256_castsi256_si128(bytes32(B, B, B, B)));
        _mm_storel_epi64((__m128i*)(cr + x), _mm256_castsi256_si128(bytes32(R, R, R, R)));
    }
#elif defined(CX_NEON)
    /* ld3 parts the pixels' R, G and B, 16 of each. */
    for (; x + 16 <= n; x += 16) {
        const uint8x16x3_t p = vld3q_u8(rgb + 3 * x);
        ycc8_neon(vget_low_u8(p.val[0]), vget_low_u8(p.val[1]), vget_low_u8(p.val[2]), y + x, cb + x, cr + x);
        ycc8_neon(vget_high_u8(p.val[0]), vget_high_u8(p.val[1]), vget_high_u8(p.val[2]), y + x + 8, cb + x + 8, cr + x + 8);
    }
#endif
    for (; x < n; x++) {
        const cx_i32 r = rgb[3 * x], g = rgb[3 * x + 1], b = rgb[3 * x + 2];
        y[x] = (cx_u8)((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
        cb[x] = (cx_u8)((-11059 * r - 21709 * g + 32768 * b + 8421375) >> 16);
        cr[x] = (cx_u8)((32768 * r - 27439 * g - 5329 * b + 8421375) >> 16);
    }
}

/* --- Huffman coding -------------------------------------------------------------------------- */

/* The bits of the entropy-coded data: `acc` holds the last n bits, fewer than 8 between puts. The
 * bytes go out as they are, and each 0xFF is followed by a 0, as JPEG stuffs it, after a call's
 * blocks are all written, by `stuff`. */
typedef struct {
    cx_u8* p;
    cx_u64 acc;
    cx_u32 n;
} bits_t;

/* Adds the low `size` bits of v, 1 to 27, which has no others set, and writes out the whole bytes:
 * eight are stored and the pointer moves on by those that are whole, so nothing waits on how
 * many there are. The output has room for the 8. */
static inline __attribute__((always_inline)) void put(bits_t* w, cx_u32 v, cx_u32 size) {
    const cx_u64 acc = w->acc << size | v;
    const cx_u32 n = w->n + size;
    const cx_u64 be = __builtin_bswap64(acc << (64 - n));
    memcpy(w->p, &be, 8);
    w->p += n >> 3;
    w->acc = acc;
    w->n = n & 7;
}

/* A 0 after each 0xFF of the bytes from a to e, which have room after them for as many as there
 * are 0xFFs: gives the new end. They're about one byte in 256, so the bytes between them move
 * back in runs, from the last. */
static cx_u8* stuff(cx_u8* a, cx_u8* e) {
    cx_usize count = 0;
    for (const cx_u8* q = a; q < e; q++) count += *q == 0xFF;
    cx_u8* const end = e + count;
    cx_u8* src = e;
    cx_u8* dst = end;
    while (count) {
        cx_u8* f = src - 1;
        while (*f != 0xFF) f--;
        const cx_usize run = (cx_usize)(src - f - 1);
        dst -= run;
        memmove(dst, f + 1, run);
        *--dst = 0;
        *--dst = 0xFF;
        src = f;
        count--;
    }
    return end;
}

static inline cx_u32 magnitude(cx_i32 v) {
    const cx_u32 a = (cx_u32)(v < 0 ? -v : v);
    return a ? 32 - (cx_u32)__builtin_clz(a) : 0;
}

cx_usize cx_jpg_huff(cx_u8* out, cx_usize len, cx_u64* st, const cx_u32* tabs, const cx_i16* coefs, const cx_u64* masks, cx_usize mcus, cx_usize luma, cx_usize ncomp) {
    bits_t w = {out + len, st[0], (cx_u32)st[1]};
    cx_i32 pred[3] = {(cx_i32)st[2], (cx_i32)st[3], (cx_i32)st[4]};
    for (cx_usize u = 0; u < mcus; u++) {
        for (cx_usize c = 0; c < ncomp; c++) {
            const cx_usize count = c == 0 ? luma : 1;
            const cx_u32* const dcode = tabs + (c == 0 ? 0 : 1024);
            const cx_u32* const dsize = dcode + 256;
            const cx_u32* const acode = dcode + 512;
            const cx_u32* const asize = dcode + 768;
            for (cx_usize i = 0; i < count; i++) {
                const cx_i32 dc = coefs[0];
                const cx_i32 diff = dc - pred[c];
                pred[c] = dc;
                const cx_u32 s = magnitude(diff);
                /* Each code and the bits after it go in at once: at most 27 bits. */
                const cx_u32 extra = (cx_u32)(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1);
                put(&w, dcode[s] << s | extra, dsize[s] + s);
                /* The nonzero coefficients, by their mask, as libjpeg-turbo finds them. */
                cx_u64 mask = *masks;
                cx_u32 last = 0;
                while (mask) {
                    const cx_u32 k = (cx_u32)__builtin_ctzll(mask);
                    mask &= mask - 1;
                    cx_u32 run = k - last - 1;
                    while (run > 15) {
                        put(&w, acode[0xF0], asize[0xF0]);
                        run -= 16;
                    }
                    const cx_i32 v = coefs[NATURAL[k]];
                    const cx_u32 m = magnitude(v);
                    const cx_u32 sym = run * 16 + m;
                    put(&w, acode[sym] << m | ((cx_u32)(v < 0 ? v - 1 : v) & ((1u << m) - 1)), asize[sym] + m);
                    last = k;
                }
                if (last < 63) put(&w, acode[0], asize[0]);
                coefs += 64;
                masks++;
            }
        }
    }
    st[0] = w.acc;
    st[1] = w.n;
    for (int c = 0; c < 3; c++) st[2 + c] = (cx_u64)(cx_i64)pred[c];
    return (cx_usize)(stuff(out + len, w.p) - out);
}

cx_usize cx_jpg_huff_end(cx_u8* out, cx_usize len, cx_u64* st) {
    /* The last byte filled out with ones, as libjpeg pads it, and the bytes still held written. */
    bits_t w = {out + len, st[0], (cx_u32)st[1]};
    put(&w, 0x7F, 7);
    st[0] = st[1] = 0;
    return (cx_usize)(stuff(out + len, w.p) - out);
}

/* --- progressive scans (CLX-213) -------------------------------------------------------------- */

/* One scan of a progressive JPEG, coded as T.81's Annex G codes it and libjpeg's jcphuff.c, which
 * Pillow writes with, does it, so the bytes are the same: a DC scan's first pass codes each DC
 * divided by 2^Al, less the one before it in the component, as a baseline scan does; its refining
 * pass writes the next bit of each, with no code. An AC scan's first pass codes the coefficients
 * Ss to Se divided by 2^Al, rounding toward zero, as runs and sizes, and a block whose last ones
 * are zero ends in an end-of-band, counted into a run of them (EOBRUN) that's coded once, before
 * the next code, or at 32,767. An AC refining pass codes the coefficients that become nonzero at
 * this bit, a 1 with its sign; the next bit of each already nonzero one goes after the next code,
 * as a correction bit, kept until then, and an end-of-band run takes its blocks' correction bits
 * with it. libjpeg writes the run out early when its bits pass 937, which changes the codes, so
 * that's here too. Zero runs of 16 aren't coded when the band's end can take them. */

/* A scan's state: the bits, the table its codes come from, and with `gather`, counts of the
 * codes instead of bits, for tables made for the scan. */
typedef struct {
    bits_t w;
    int gather;
    cx_u64* counts;
    const cx_u32* code;
    const cx_u32* size;
    cx_u32 eobrun;
    cx_u32 be;              /* correction bits held for the end-of-band run */
    cx_u8 corr[1000 + 64];  /* those, then the block's own */
} prog_t;

static inline void p_code(prog_t* P, cx_u32 sym) {
    if (P->gather) P->counts[sym]++;
    else put(&P->w, P->code[sym], P->size[sym]);
}

/* The low n bits of v, n from 1 to 16. */
static inline void p_bits(prog_t* P, cx_u32 v, cx_u32 n) {
    if (!P->gather) put(&P->w, v & ((1u << n) - 1), n);
}

/* Correction bits, 24 at a time. */
static void p_corr(prog_t* P, const cx_u8* b, cx_u32 n) {
    if (P->gather) return;
    cx_u32 i = 0;
    for (; i + 24 <= n; i += 24) {
        cx_u32 v = 0;
        for (cx_u32 j = 0; j < 24; j++) v = v << 1 | b[i + j];
        put(&P->w, v, 24);
    }
    if (i < n) {
        cx_u32 v = 0;
        for (cx_u32 j = i; j < n; j++) v = v << 1 | b[j];
        put(&P->w, v, n - i);
    }
}

/* The end-of-band run, if there is one: a code for its size in bits less one, then its low bits,
 * then the correction bits held with it. */
static void p_eobrun(prog_t* P) {
    if (!P->eobrun) return;
    const cx_u32 nb = 31 - (cx_u32)__builtin_clz(P->eobrun);
    p_code(P, nb << 4);
    if (nb) p_bits(P, P->eobrun, nb);
    P->eobrun = 0;
    p_corr(P, P->corr, P->be);
    P->be = 0;
}

/* The n blocks of 64 coefficients of `coefs` in row order, packed for the scans: each one's DC
 * into dc, and its nonzero AC ones, in zigzag order as its mask has them, into vals from offs[i].
 * A scan then reads a few hundred bytes a row of blocks rather than every block's 128, and ten
 * scans, each read twice, would otherwise read them all from beyond the L2 cache. Gives how many
 * went into vals, which has room for every mask's bits. */
cx_usize cx_jpg_pack(const cx_i16* coefs, const cx_u64* masks, cx_usize n, cx_i16* dc, cx_i16* vals, cx_u32* offs) {
    cx_usize at = 0;
    for (cx_usize i = 0; i < n; i++, coefs += 64) {
        dc[i] = coefs[0];
        offs[i] = (cx_u32)at;
        cx_u64 m = masks[i];
        while (m) {
            vals[at++] = coefs[NATURAL[__builtin_ctzll(m)]];
            m &= m - 1;
        }
    }
    return at;
}

/* The scan of blocks order[0..n), packed by cx_jpg_pack: dc, vals and offs, with each block's
 * nonzero AC coefficients as bits of masks in zigzag order. comp[i] is a block's component, 0 to
 * 2, whose DC goes after that component's last and whose table is 0 for the first component and 1
 * for the others; tabs holds 1024 per table as cx_jpg_huff's. With gather it counts each table's
 * codes into counts, 256 a table, and writes nothing; otherwise it writes the scan's bytes from
 * out + len, padded to a byte and stuffed, and gives the new length. out has room for 27 bits a
 * coefficient of the band and 94 more a block, doubled, and 1024 bytes more. The AC passes go
 * through the nonzero coefficients by their masks: a zero one is zero at every bit. */
cx_usize cx_jpg_scan(cx_u8* out, cx_usize len, cx_u32 gather, cx_u64* counts, const cx_u32* tabs, const cx_i16* dc, const cx_i16* vals,
                     const cx_u32* offs, const cx_u64* masks, const cx_u32* order, const cx_u8* comp, cx_usize n, cx_u32 ss, cx_u32 se,
                     cx_u32 ah, cx_u32 al) {
    static prog_t zero;
    prog_t* const P = (prog_t*)cx_alloc(sizeof(prog_t));
    *P = zero;
    P->w.p = out + len;
    P->gather = (int)gather;
    if (ss == 0) {
        if (ah == 0) {
            cx_i32 pred[3] = {0, 0, 0};
            for (cx_usize i = 0; i < n; i++) {
                const cx_u32 c = comp[i], t = c ? 1 : 0;
                P->counts = counts + 256 * t;
                P->code = tabs + 1024 * t;
                P->size = P->code + 256;
                const cx_i32 d = (cx_i32)dc[order[i]] >> al;
                const cx_i32 diff = d - pred[c];
                pred[c] = d;
                const cx_u32 s = magnitude(diff);
                p_code(P, s);
                if (s) p_bits(P, (cx_u32)(diff < 0 ? diff - 1 : diff), s);
            }
        } else {
            for (cx_usize i = 0; i < n; i++) p_bits(P, (cx_u32)((cx_i32)dc[order[i]] >> al), 1);
        }
    } else {
        const cx_u32 t = n && comp[0] ? 1 : 0;
        P->counts = counts + 256 * t;
        P->code = tabs + 1024 * t + 512;
        P->size = P->code + 256;
        /* The band's bits of a mask. */
        const cx_u64 below = ((cx_u64)1 << ss) - 1;
        const cx_u64 band = (se == 63 ? ~(cx_u64)0 : ((cx_u64)1 << (se + 1)) - 1) & ~below;
        for (cx_usize i = 0; i < n; i++) {
            const cx_u32 at_block = order[i];
            const cx_u64 full = masks[at_block];
            /* The band's values follow those of the coefficients before it. */
            const cx_i16* b = vals + offs[at_block] + __builtin_popcountll(full & below);
            cx_u64 m = full & band;
            if (ah == 0) {
                /* The last coefficient coded, and each run of zeros from there. */
                cx_u32 last = ss - 1;
                while (m) {
                    const cx_u32 k = (cx_u32)__builtin_ctzll(m);
                    m &= m - 1;
                    const cx_i32 v = *b++;
                    const cx_u32 a = (cx_u32)(v < 0 ? -v : v) >> al;
                    if (!a) continue;
                    cx_u32 r = k - last - 1;
                    p_eobrun(P);
                    while (r > 15) {
                        p_code(P, 0xF0);
                        r -= 16;
                    }
                    const cx_u32 s = 32 - (cx_u32)__builtin_clz(a);
                    p_code(P, (r << 4) + s);
                    p_bits(P, v < 0 ? ~a : a, s);
                    last = k;
                }
                if (last < se) {
                    if (++P->eobrun == 0x7FFF) p_eobrun(P);
                }
            } else {
                /* The coefficients that aren't zero at this bit, each one's size there, and the
                 * last that becomes nonzero at it. */
                cx_u8 at[64], pos[64];
                cx_u32 abs[64], count = 0, eob = 0;
                while (m) {
                    const cx_u32 k = (cx_u32)__builtin_ctzll(m);
                    m &= m - 1;
                    const cx_i32 v = *b++;
                    const cx_u32 a = (cx_u32)(v < 0 ? -v : v) >> al;
                    if (!a) continue;
                    at[count] = (cx_u8)k;
                    pos[count] = v >= 0;
                    abs[count++] = a;
                    if (a == 1) eob = k;
                }
                /* This block's correction bits go after those held, until a code takes them. The
                 * run of zeros doesn't count the coefficients that were already nonzero. */
                cx_u32 base = P->be, br = 0, r = 0, prev = ss - 1;
                for (cx_u32 j = 0; j < count; j++) {
                    const cx_u32 k = at[j], a = abs[j];
                    r += k - prev - 1;
                    prev = k;
                    while (r > 15 && k <= eob) {
                        p_eobrun(P);
                        p_code(P, 0xF0);
                        r -= 16;
                        p_corr(P, P->corr + base, br);
                        base = 0;
                        br = 0;
                    }
                    if (a > 1) {
                        P->corr[base + br++] = (cx_u8)(a & 1);
                        continue;
                    }
                    p_eobrun(P);
                    p_code(P, (r << 4) + 1);
                    p_bits(P, pos[j], 1);
                    p_corr(P, P->corr + base, br);
                    base = 0;
                    br = 0;
                    r = 0;
                }
                r += se - prev;
                if (r || br) {
                    P->eobrun++;
                    P->be += br;
                    if (P->eobrun == 0x7FFF || P->be > 1000 - 64 + 1) p_eobrun(P);
                }
            }
        }
        p_eobrun(P);
    }
    cx_usize got = len;
    if (!gather) {
        put(&P->w, 0x7F, 7);
        got = (cx_usize)(stuff(out + len, P->w.p) - out);
    }
    cx_free(P, sizeof(prog_t));
    return got;
}

/* --- downsampling ---------------------------------------------------------------------------- */

/* 2n samples of r0, and of r1 if it isn't null, averaged into n. */
static inline __attribute__((always_inline)) void down(const cx_u8* r0, const cx_u8* r1, cx_usize n, cx_u8* out) {
    cx_usize x = 0;
#if defined(__AVX2__)
    const V ones = _mm256_set1_epi8(1);
    /* The rounding alternates: 1 and 2 over four samples, 0 and 1 over two. */
    const V bias = r1 ? _mm256_set1_epi32(0x00020001) : _mm256_set1_epi32(0x00010000);
    for (; x + 16 <= n; x += 16) {
        V s = _mm256_maddubs_epi16(_mm256_loadu_si256((const V*)(r0 + 2 * x)), ones);
        if (r1) s = _mm256_add_epi16(s, _mm256_maddubs_epi16(_mm256_loadu_si256((const V*)(r1 + 2 * x)), ones));
        s = r1 ? _mm256_srli_epi16(_mm256_add_epi16(s, bias), 2) : _mm256_srli_epi16(_mm256_add_epi16(s, bias), 1);
        _mm_storeu_si128((__m128i*)(out + x), _mm_packus_epi16(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1)));
    }
#elif defined(CX_NEON)
    /* Pairs added into 16-bit lanes, with the same alternating rounding. */
    const uint16x8_t bias = vreinterpretq_u16_u32(vdupq_n_u32(r1 ? 0x00020001u : 0x00010000u));
    for (; x + 16 <= n; x += 16) {
        uint16x8_t s0 = vpaddlq_u8(vld1q_u8(r0 + 2 * x)), s1 = vpaddlq_u8(vld1q_u8(r0 + 2 * x + 16));
        if (r1) {
            s0 = vpadalq_u8(s0, vld1q_u8(r1 + 2 * x));
            s1 = vpadalq_u8(s1, vld1q_u8(r1 + 2 * x + 16));
        }
        s0 = vaddq_u16(s0, bias);
        s1 = vaddq_u16(s1, bias);
        const uint8x16_t o = r1 ? vcombine_u8(vshrn_n_u16(s0, 2), vshrn_n_u16(s1, 2)) : vcombine_u8(vshrn_n_u16(s0, 1), vshrn_n_u16(s1, 1));
        vst1q_u8(out + x, o);
    }
#endif
    for (; x < n; x++) {
        if (r1) {
            out[x] = (cx_u8)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 1 + (cx_i32)(x & 1)) >> 2);
        } else {
            out[x] = (cx_u8)((r0[2 * x] + r0[2 * x + 1] + (cx_i32)(x & 1)) >> 1);
        }
    }
}

void cx_jpg_down22(const cx_u8* r0, const cx_u8* r1, cx_usize n, cx_u8* out) { down(r0, r1, n, out); }

void cx_jpg_down21(const cx_u8* r0, cx_usize n, cx_u8* out) { down(r0, 0, n, out); }
