---
title: std.math
description: Fast math functions, accurate to about half an ULP and the same on every platform - exp, log, pow, the trigonometric functions and the rest.
---

# `std.math`

Math functions that are fast, accurate to about half an ULP, and the same on every platform, since they're written in Calx and call no system library (D97). For Java's `StrictMath` results bit for bit, which vanilla's behaviour depends on, use [`std.java.math`](java-math.md): it has the same functions under the same names.

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
| `sin(x)`, `cos(x)`, `tan(x)` | In radians. Infinity and NaN give NaN |
| `asin(x)`, `acos(x)`, `atan(x)` | The inverses. Outside [-1, 1], `asin` and `acos` give NaN |
| `atan2(y, x)` | The angle of (`x`, `y`), from -pi to pi. `y` comes first, as in Java and C |
| `hypot(x, y)`, `cbrt(x)` | sqrt(xÂ² + yÂ²) without overflow, and the cube root, negative numbers included |
| `sinh(x)`, `cosh(x)`, `tanh(x)` | The hyperbolic functions |
| `PI`, `E` | The doubles closest to pi and e |

- Special values are Java's in every function: `pow(x, 0.0)` is `1.0` even for NaN, `pow(Â±1.0, Â±Infinity)` is NaN, and a negative `x` to a power that isn't an integer is NaN.
- `sqrt`, `floor`, `ceil`, `abs` and `mul_add` are methods on the float types, since IEEE 754 defines their results exactly ([floats](../reference/floats.md#methods)).

## Accuracy

`exp`, `log` and `pow` are Calx's own. They were checked against values computed to 45 digits with Python's `decimal`, on 1.6 million arguments: random ones across each function's range, arguments near 1, subnormal arguments and results, and results near overflow.

| Function | Worst error | Correctly rounded | `StrictMath` |
| --- | --- | --- | --- |
| `exp` | 0.5035 ULP | 99.96% | under 1 ULP |
| `log` | 0.5054 ULP | 99.99% | under 1 ULP |
| `pow` | 0.5044 ULP | 99.94% | under 1 ULP |

So they're more accurate than `StrictMath`, which is fdlibm: `pow(7.0, 22.0)` is the double nearest 7Â²Â², `3.9098210485829883E18`, where `StrictMath.pow` gives the one below it. The other functions call `std.java.math`, which is fdlibm too, and give `StrictMath`'s results; only `std.java.math` promises that, so a faster algorithm can replace any of them here.

## Speed

Nanoseconds per call on 2 million random arguments, best of 7, from [`bench/math`](../../bench/math), on an i9-13900K pinned to one P-core. The C library is what Rust's `f64::exp`, `ln`, `powf` and the rest call: the Windows CRT on Windows, glibc on Linux.

| Function | Calx, Windows | Windows CRT | Calx, Linux | glibc 2.43 |
| --- | --- | --- | --- | --- |
| `exp` | **1.27** | 2.46 | **1.29** | 2.23 |
| `log` | **1.93** | 2.33 | **1.91** | 2.05 |
| `pow` | **5.94** | 6.30 | **5.70** | 6.56 |
| `sin` | 8.15 | **7.41** | **8.06** | 11.69 |
| `cos` | 8.20 | **7.15** | **8.07** | 10.22 |
| `tan` | 10.22 | **8.73** | **10.27** | 12.00 |
| `atan2` | **10.01** | 14.96 | **9.91** | 15.75 |
| `acos` | **5.99** | 8.43 | **5.82** | 10.96 |

`std.java.math`'s `exp`, `log` and `pow`, which must follow fdlibm's steps to get its last bit, take 3.72, 3.64 and 29.5 on Windows. `sin`, `cos` and `tan` are fdlibm's here too, which the Windows CRT beats; a faster algorithm for them is planned.

## How `exp`, `log` and `pow` work

They follow Tang's table-driven method, with tables of 128 entries and polynomials that [`tools/math_tables.py`](../../tools/math_tables.py) generates and checks, at 80 digits.

- **`exp`** writes `x` as kÂ·ln2/128 + r, with |r| â‰¤ ln2/256. A table gives 2^(k/128) and a degree 5 polynomial gives e^r. Each table entry is laid out so that adding `k << 45` to its bits gives 2^(k/128), exponent and all, as in ARM's optimized-routines.
- **`log`** writes `x` as 2^kÂ·z and picks one of 128 values c near z, with log(x) = kÂ·ln2 + log(c) + log1p(z/c - 1). Each 1/c is a multiple of 2^-7 or 2^-8, so z/c - 1 is exact in one fused multiply-add, and the two c next to 1 are 1 itself, so `log` needs no separate path near 1.
- **`pow`** computes log(x) as two doubles, accurate to about 2^-68, multiplies by `y` exactly, and takes `exp` of the sum. Most special cases cost one branch.
- Every rounding they depend on is a fused multiply-add or a plain operation, both of which IEEE 754 defines exactly, so debug and release builds, and every machine, give the same bits.

`std.math` is written in Calx: [std/math.calx](../../std/math.calx).
