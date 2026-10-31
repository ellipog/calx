---
title: std.math
description: Fast math functions, accurate to about half an ULP and the same on every platform - exp, log, pow, the trigonometric functions and the rest.
---

# `std.math`

Math functions that are fast, accurate to about half an ULP, and the same on every platform, since they're written in Calx and call no system library (D97). For Java's `StrictMath` results bit for bit, use [`std.java.math`](java-math.md): it has the same functions under the same names.

```calx
import std.math.{PI, cos, exp, log, pow, sin}

fn main() {
    println("{exp(1.0)} {log(10.0)} {pow(2.0, 0.5)}")    // out: 2.718281828459045 2.302585092994046 1.4142135623730951
    println("{sin(PI / 6.0)} {cos(PI)}")                  // out: 0.49999999999999994 -1.0
    println("{pow(-8.0, 1.0 / 3.0)} {pow(-2.0, 3.0)}")    // out: NaN -8.0
}
```

| Function | Gives |
| --- | --- |
| `exp(x)` | e to the power `x`. `Infinity` above about 709.78, `0.0` below about -745.13 |
| `log(x)` | The natural logarithm. `log(0.0)` is `-Infinity`, and a negative number or NaN gives NaN |
| `pow(x, y)` | `x` to the power `y`, exact when the result is a double |
| `log10(x)`, `log1p(x)`, `expm1(x)` | The base 10 logarithm, `log(1 + x)` and `exp(x) - 1`, the last two accurate near zero |
| `sin(x)`, `cos(x)`, `tan(x)` | In radians. Infinity and NaN give NaN, and `sin` and `tan` keep the sign of zero |
| `asin(x)`, `acos(x)`, `atan(x)` | The inverses. Outside [-1, 1], `asin` and `acos` give NaN |
| `atan2(y, x)` | The angle of (`x`, `y`), from -pi to pi. `y` comes first, as in Java and C |
| `hypot(x, y)`, `cbrt(x)` | sqrt(x² + y²) without overflow, and the cube root, negative numbers included |
| `sinh(x)`, `cosh(x)`, `tanh(x)` | The hyperbolic functions |
| `PI`, `E` | The doubles closest to pi and e |

- Special values are Java's in every function: `pow(x, 0.0)` is `1.0` even for NaN, `pow(±1.0, ±Infinity)` is NaN, and a negative `x` to a power that isn't an integer is NaN.
- `sqrt`, `floor`, `ceil`, `abs` and `mul_add` are methods on the float types, since IEEE 754 defines their results exactly ([floats](../reference/floats.md#methods)).

## Accuracy

`exp`, `log`, `pow`, `sin`, `cos` and `tan` are Calx's own. They were checked against values computed to 45 or 50 digits with Python's `decimal`: 1.6 million arguments for the first three, with random ones across each function's range, arguments near 1, subnormal arguments and results, and results near overflow; 718,000 for the others, with random ones up to 2^20, and the 3,000 doubles below 2^20 nearest a multiple of pi/2, where the reduction is hardest. The last column is fdlibm's error, which `StrictMath` and `std.java.math` give, on the same arguments.

| Function | Worst error | Correctly rounded | `StrictMath` |
| --- | --- | --- | --- |
| `exp` | 0.5035 ULP | 99.96% | under 1 ULP |
| `log` | 0.5054 ULP | 99.99% | under 1 ULP |
| `pow` | 0.5044 ULP | 99.94% | under 1 ULP |
| `sin` | 0.5328 ULP | 99.84% | 0.8113 ULP, 97.16% |
| `cos` | 0.5327 ULP | 99.80% | 0.8090 ULP, 97.01% |
| `tan` | 0.5528 ULP | 99.74% | 0.8041 ULP, 98.22% |

So they're more accurate than `StrictMath`: `pow(7.0, 22.0)` is the double nearest 7²², `3.9098210485829883E18`, where `StrictMath.pow` gives the one below it. `sin`, `cos` and `tan` of 2^20 or more call `std.java.math`, as do the other functions, so they give `StrictMath`'s results; only `std.java.math` promises that, so a faster algorithm can replace any of them here.

## Speed

Nanoseconds per call on 2 million random arguments, best of 7 runs in each of two processes, from [`bench/math`](../../bench/math), on an i9-13900K pinned to one P-core. The trigonometric functions' arguments are in [-10, 10]. The C library is what Rust's `f64::exp`, `ln`, `powf`, `sin` and the rest call: the Windows CRT on Windows, glibc on Linux.

| Function | Calx, Windows | Windows CRT | Calx, Linux | glibc 2.43 |
| --- | --- | --- | --- | --- |
| `exp` | **1.38** | 2.58 | **1.56** | 2.26 |
| `log` | **1.86** | 2.54 | **1.90** | 2.14 |
| `pow` | **5.96** | 6.42 | **5.71** | 6.61 |
| `sin` | **4.34** | 7.22 | **4.19** | 11.68 |
| `cos` | **4.42** | 7.06 | **4.22** | 10.10 |
| `tan` | **8.11** | 8.47 | **7.98** | 12.04 |
| `atan2` | **10.39** | 15.04 | **10.05** | 16.04 |
| `acos` | **5.83** | 7.88 | **6.05** | 10.77 |

`std.java.math`, which must follow fdlibm's steps to get its last bit, takes 3.72, 3.64 and 29.5 for `exp`, `log` and `pow`, and 8.15, 8.20 and 10.22 for `sin`, `cos` and `tan`, on Windows.

## How they work

`exp`, `log` and `pow` follow Tang's table-driven method, with tables of 128 entries, and `sin`, `cos` and `tan` reduce by pi/2. [`tools/math_tables.py`](../../tools/math_tables.py) generates and checks every table and polynomial, at 80 digits.

- **`exp`** writes `x` as k·ln2/128 + r, with |r| ≤ ln2/256. A table gives 2^(k/128) and a degree 5 polynomial gives e^r. Each table entry is laid out so that adding `k << 45` to its bits gives 2^(k/128), exponent and all, as in ARM's optimized-routines.
- **`log`** writes `x` as 2^k·z and picks one of 128 values c near z, with log(x) = k·ln2 + log(c) + log1p(z/c - 1). Each 1/c is a multiple of 2^-7 or 2^-8, so z/c - 1 is exact in one fused multiply-add, and the two c next to 1 are 1 itself, so `log` needs no separate path near 1.
- **`pow`** computes log(x) as two doubles, accurate to about 2^-68, multiplies by `y` exactly, and takes `exp` of the sum. Most special cases cost one branch.
- **`sin` and `cos`** write `x` as k·pi/2 + r, with |r| ≤ pi/4, using pi/2 as three doubles, so r is exact to about 2^-140 and comes as two doubles. The result is ±sin(r) or ±cos(r), as k mod 4 says, and each of those is a + a·r²·c + a·r⁴·Q(r²), with a = r or 1, so one sum computes all four, with what differs read from a table. On random arguments, a branch on k would be mispredicted three times in four; this way there's one branch, for |x| ≥ 2^20. The first two terms are summed as two doubles, so the only large rounding is the last.
- **`tan`** is sin(r + k·pi/2) / sin(r + (k+1)·pi/2), each as two doubles from the same sum, divided once and corrected by the remainder.
- Every rounding they depend on is a fused multiply-add or a plain operation, both of which IEEE 754 defines exactly, so debug and release builds, and every machine, give the same bits.

`std.math` is written in Calx: [std/math.calx](../../std/math.calx).
