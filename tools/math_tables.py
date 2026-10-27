"""Generates the tables and polynomials of std.math's exp, log and pow (std/math.calx).

    py -I tools/math_tables.py > tables.calx

Everything is computed with Python's decimal module at 80 digits, then rounded to doubles. The
polynomials are minimax fits from a Remez exchange. The script asserts the properties the code
relies on, listed with each table.
"""
import decimal
import math
import struct
import sys
from decimal import Decimal as D

decimal.getcontext().prec = 80

ONE = D(1)
LN2 = D(2).ln()
N = 128    # entries in each table


def bits(f):
    return struct.unpack("<Q", struct.pack("<d", f))[0]


def from_bits(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def to_grid(v, g):
    """v rounded to the nearest multiple of 2^-g."""
    s = D(2) ** g
    return (v * s).to_integral_value(rounding=decimal.ROUND_HALF_EVEN) / s


def lit(f):
    """A Calx literal that reads back as exactly f."""
    r = repr(f)
    if "e" in r and "." not in r.split("e")[0]:
        m, e = r.split("e")
        r = m + ".0e" + e
    return r


def log2_of(v):
    return float(abs(v).ln() / LN2)


# --- Remez -------------------------------------------------------------------------------------

def solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c and m[r][c] != 0:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def horner(cs, x):
    s = D(0)
    for c in reversed(cs):
        s = s * x + c
    return s


def remez(f, lo, hi, deg, iters=12, grid=6000):
    """The polynomial of degree deg with the least maximum absolute error from f on [lo, hi]."""
    n = deg + 2
    xs = [(lo + hi) / 2 + (hi - lo) / 2 * D(math.cos(math.pi * (n - 1 - i) / (n - 1))) for i in range(n)]
    pts = [lo + (hi - lo) * i / grid for i in range(grid + 1)]
    fv = [f(p) for p in pts]
    cs = []
    errs = []
    for _ in range(iters):
        a = [[x ** j if j else ONE for j in range(deg + 1)] + [D(-1) ** i] for i, x in enumerate(xs)]
        cs = solve(a, [f(x) for x in xs])[:deg + 1]
        errs = [fv[i] - horner(cs, p) for i, p in enumerate(pts)]
        # The extreme error of each run of one sign.
        ext = []
        i = 0
        while i < len(pts):
            j = i
            best = i
            while j < len(pts) and (errs[j] >= 0) == (errs[i] >= 0):
                if abs(errs[j]) > abs(errs[best]):
                    best = j
                j += 1
            ext.append(best)
            i = j
        while len(ext) > n:
            if abs(errs[ext[0]]) < abs(errs[ext[-1]]):
                ext.pop(0)
            else:
                ext.pop()
        if len(ext) < n:
            break
        xs = [pts[k] for k in ext]
    # The error of the coefficients as doubles, which is what the code evaluates.
    rounded = [D(float(c)) for c in cs]
    worst = max(abs(fv[i] - horner(rounded, p)) for i, p in enumerate(pts))
    return [float(c) for c in cs], worst


# --- exp ---------------------------------------------------------------------------------------
#
# e^x = 2^(k/N) e^r, with k = round(x N / ln2) and |r| <= ln2 / 2N. EXP_T holds, for each j < N,
# the bits of 2^(j/N) as a double, less j << 45, and its relative tail: 2^(j/N) is
# T (1 + tail). Adding k << 45 to the first gives 2^(k/N), exponent and all.

exp_t = []
for j in range(N):
    v = (D(j) / N * LN2).exp()
    t = float(v)
    exp_t.append(bits(float(v / D(t) - ONE)))
    exp_t.append((bits(t) - (j << 45)) & 0xffffffffffffffff)
neg_ln2hi_n = float(-LN2 / N)
neg_ln2lo_n = float(-LN2 / N - D(neg_ln2hi_n))
rmax = LN2 / (2 * N) * D("1.0001")


def exp_q(r):
    # (e^r - 1 - r) / r^2 = sum r^(k-2) / k!
    s = D(0)
    f = D(1)
    p = D(1)
    for k in range(2, 30):
        f *= k
        s += p / f
        p *= r
    return s


exp_c, exp_err = remez(exp_q, -rmax, rmax, 3)

# --- log ---------------------------------------------------------------------------------------
#
# x = 2^k z, with z in [0x1.6p-1, 0x1.6p0) and the top 7 bits of z's mantissa, past LOG_OFF,
# choosing one of N subintervals. log(x) = k ln2 + log(c) + log1p(r), with r = z / c - 1. Each
# entry of LOG_T is 1/c, then -log(1/c) as hi + lo:
#
# - 1/c is a multiple of 2^-7 below 1 and of 2^-8 above, so z * (1/c) - 1 is exact in one fused
#   multiply-add for every z in the subinterval: |r| <= 2^-7 keeps it in 53 bits.
# - The two subintervals next to 1 have c = 1, so log(x) near 1 is r + log1p(r) - r with no
#   cancellation, and needs no path of its own.
# - hi is a multiple of 2^-42, like LOG_LN2_HI, so k ln2_hi + hi is exact for every k.
# - |hi| >= |r|, so hi + r can be split into a sum and its exact rounding error.

OFF = 0x3fe6000000000000
log_t = []
r_lo = D(0)
r_hi = D(0)
for i in range(N):
    a = D(from_bits(OFF + (i << 45)))
    b = D(from_bits(OFF + ((i + 1) << 45)))    # the subinterval is [a, b)
    g = 7 if a < 1 else 8
    if a == 1 or b == 1:
        invc = ONE
    else:
        near = to_grid(2 / (a + b), g)
        cands = [near + D(d) / D(2) ** g for d in range(-3, 4)]
        invc = min(cands, key=lambda iv: max(abs(a * iv - 1), abs(b * iv - 1)))
    lo_end = a * invc - 1
    hi_end = b * invc - 1
    assert max(abs(lo_end), abs(hi_end)) <= D(2) ** -7, i
    r_lo = min(r_lo, lo_end)
    r_hi = max(r_hi, hi_end)
    logc = -invc.ln() if invc != 1 else D(0)
    logc_hi = to_grid(logc, 42)
    assert invc == 1 or abs(logc_hi) >= max(abs(lo_end), abs(hi_end)), i
    log_t += [float(invc), float(logc_hi), float(logc - logc_hi)]
    assert D(log_t[-3]) == invc and D(log_t[-2]) == logc_hi
ln2_hi = to_grid(LN2, 42)
ln2_lo = float(LN2 - ln2_hi)


def log_q(r):
    # (log1p(r) - r + r^2/2) / r^3 = sum_{n>=3} (-1)^(n+1) r^(n-3) / n
    s = D(0)
    p = D(1)
    for n in range(3, 80):
        s += (p / n) if n % 2 == 1 else -(p / n)
        p *= r
    return s


qlo = r_lo * D("1.001")
qhi = r_hi * D("1.001")
log_c, log_err = remez(log_q, qlo, qhi, 5)
pow_c, pow_err = remez(log_q, qlo, qhi, 6)

# --- sin and cos -------------------------------------------------------------------------------
#
# x = k pi/2 + r, with k = round(x 2/pi) and |r| <= pi/4, for |x| < 2^20. pi/2 is split into
# three doubles, so x - k pio2_1 is exact in one fused multiply-add (it's a multiple of 2^-53
# under 1), and r is carried as hi + lo. The kernel works out sin(r) or cos(r), and its sign, from
# k mod 4, with no branch: TRIG_K has, for each quadrant, what the kernel multiplies by, then the
# polynomial. sin(r) = r - r^3/6 + r^5 Q(r^2), and cos(r) = 1 - r^2/2 + r^4 Q(r^2).


def atan_inv(n):
    # atan(1/n) = sum (-1)^k / ((2k + 1) n^(2k + 1))
    s = D(0)
    p = ONE / n
    k = 0
    while p > D(10) ** -85:
        s += p / (2 * k + 1) if k % 2 == 0 else -(p / (2 * k + 1))
        p /= n * n
        k += 1
    return s


PI = 16 * atan_inv(5) - 4 * atan_inv(239)
PIO2 = PI / 2
pio2_1 = float(PIO2)
pio2_2 = float(PIO2 - D(pio2_1))
pio2_3 = float(PIO2 - D(pio2_1) - D(pio2_2))
assert bits(pio2_1) & 0x7ff0000000000000 == 0x3ff0000000000000    # in [1, 2), so k pio2_1 is a multiple of 2^-52
trig_zmax = (PI / 4) ** 2 * D("1.0001")


def sin_q(z):
    # (sin(r) - r + r^3/6) / r^5 = sum_{n>=2} (-1)^n z^(n-2) / (2n+1)!
    s = D(0)
    f = D(120)
    p = ONE
    for n in range(2, 40):
        s += p / f if n % 2 == 0 else -(p / f)
        p *= z
        f *= (2 * n + 2) * (2 * n + 3)
    return s


def cos_q(z):
    # (cos(r) - 1 + r^2/2) / r^4 = sum_{n>=2} (-1)^n z^(n-2) / (2n)!
    s = D(0)
    f = D(24)
    p = ONE
    for n in range(2, 40):
        s += p / f if n % 2 == 0 else -(p / f)
        p *= z
        f *= (2 * n + 1) * (2 * n + 2)
    return s


TRIG_DEG = 5
sin_c, sin_err = remez(sin_q, D(0), trig_zmax, TRIG_DEG)
cos_c, cos_err = remez(cos_q, D(0), trig_zmax, TRIG_DEG)
sixth_hi = float(-ONE / 6)
sixth_lo = float(-ONE / 6 - D(sixth_hi))
# Each quadrant: a = AR r + AC is r or 1, signed; g = (GR r + GC)(1 + GZ r^2) approximates what
# r_lo is multiplied by, the derivative cos(r) or -sin(r), signed; then -1/6 or -1/2 as hi + lo,
# and Q; and zeros, so each row is 16 long.
trig_k = []
for q in range(4):
    sign = 1.0 if q < 2 else -1.0
    if q % 2 == 0:
        row = [sign, 0.0, 0.0, sign, -0.5, sixth_hi, sixth_lo] + sin_c
    else:
        row = [0.0, sign, -sign, 0.0, float(-ONE / 6), -0.5, 0.0] + cos_c
    trig_k += row + [0.0] * (16 - len(row))
    assert len(row) == 7 + TRIG_DEG + 1

# --- output ------------------------------------------------------------------------------------

print(f"r for log is in [{float(r_lo)}, {float(r_hi)}]", file=sys.stderr)
print(f"exp: (e^r - 1 - r) / r^2 within 2^{log2_of(exp_err):.1f}", file=sys.stderr)
print(f"log: q within 2^{log2_of(log_err):.1f}; pow: q within 2^{log2_of(pow_err):.1f}", file=sys.stderr)
print(f"sin: Q within 2^{log2_of(sin_err):.1f}; cos: Q within 2^{log2_of(cos_err):.1f}", file=sys.stderr)

out = ["// Generated by tools/math_tables.py.", ""]
out.append(f"const EXP_INVLN2N: f64 = {lit(float(N / LN2))}    // N / ln2")
out.append(f"const EXP_SHIFT: f64 = {lit(6755399441055744.0)}    // 0x1.8p52, for rounding to an integer")
out.append(f"const EXP_NEG_LN2HI_N: f64 = {lit(neg_ln2hi_n)}    // -ln2 / N, as hi + lo")
out.append(f"const EXP_NEG_LN2LO_N: f64 = {lit(neg_ln2lo_n)}")
out.append(f"// e^r - 1 - r = r^2 (c2 + c3 r + c4 r^2 + c5 r^3), within 2^{log2_of(exp_err):.1f} r^2 for |r| <= ln2 / 256")
for k, c in enumerate(exp_c):
    out.append(f"const EXP_C{k + 2}: f64 = {lit(c)}")
out.append("const EXP_T: [u64; 256] = [")
for j in range(0, 2 * N, 4):
    out.append("    " + ", ".join(f"0x{v:016x}" for v in exp_t[j:j + 4]) + ",")
out.append("]")
out.append("")
out.append(f"const LOG_OFF: u64 = 0x{OFF:016x}    // 0x1.6p-1")
out.append(f"const LOG_LN2_HI: f64 = {lit(float(ln2_hi))}    // ln2, as a multiple of 2^-42 + lo")
out.append(f"const LOG_LN2_LO: f64 = {lit(ln2_lo)}")
out.append(f"// (log1p(r) - r + r^2 / 2) / r^3, within 2^{log2_of(log_err):.1f} for log's r, and to one more")
out.append(f"// degree, within 2^{log2_of(pow_err):.1f}, for pow")
for k, c in enumerate(log_c):
    out.append(f"const LOG_Q{k}: f64 = {lit(c)}")
for k, c in enumerate(pow_c):
    out.append(f"const POW_Q{k}: f64 = {lit(c)}")
out.append("// 1 / c, then log(c) as hi + lo")
out.append(f"const LOG_T: [f64; {3 * N}] = [")
for i in range(N):
    out.append("    " + ", ".join(lit(v) for v in log_t[3 * i:3 * i + 3]) + ",")
out.append("]")
out.append("")
out.append(f"const TRIG_INV_PIO2: f64 = {lit(float(2 / PI))}    // 2 / pi")
out.append(f"const TRIG_NEG_PIO2_1: f64 = {lit(-pio2_1)}    // pi / 2 as three doubles, the first negated")
out.append(f"const TRIG_PIO2_2: f64 = {lit(pio2_2)}")
out.append(f"const TRIG_PIO2_3: f64 = {lit(pio2_3)}")
out.append(f"// For each quadrant: AR, AC, GR, GC, GZ, then -1/6 or -1/2 as hi + lo, then Q, within")
out.append(f"// 2^{log2_of(sin_err):.1f} for sin and 2^{log2_of(cos_err):.1f} for cos")
out.append("const TRIG_K: [f64; 64] = [")
for q in range(4):
    row = trig_k[16 * q:16 * q + 16]
    out.append("    " + ", ".join(lit(v) for v in row[:7]) + ",")
    out.append("    " + ", ".join(lit(v) for v in row[7:]) + ",")
out.append("]")
print("\n".join(out))
