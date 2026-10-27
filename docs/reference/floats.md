---
title: Floats
description: f32 and f64 arithmetic, printing and methods, matching Java's float and double.
---

# Floats

`f32` and `f64` are IEEE 754 binary32 and binary64, Java's `float` and `double`. Arithmetic gives Java's results bit for bit.

| Rule | Calx | Emitted C |
| --- | --- | --- |
| Format | IEEE 754, round to nearest even, no flush-to-zero | SSE2 on x86-64, never x87 |
| Contraction | No fused multiply-add and no reassociation. `a * b + c` rounds twice, as in Java. `a.mul_add(b, c)` asks for the fused one | `-ffp-contract=off`, and never `-ffast-math` |
| `%` | The remainder of truncating division, Java's `%` on doubles | `fmod` |
| Comparison | IEEE: NaN is unequal to everything, including itself, and `-0.0 == 0.0` | |
| Mixing | There's none. `f32 * i32` doesn't compile, and Java's implicit widening is written out: `f64(meters * f32.nearest(packets))` | |

## Literals

- An unsuffixed float literal takes its type from where it's used, and is an `f64` when nothing decides. `1.0e-5f32` is an `f32`.
- A literal keeps its type, so Java's `1.0E-5F` widened to a double is written `f64(1.0e-5f32)`.
- A float literal needs a fraction or an exponent: `5.0`, not `5`, and not `5f32`.
- A literal too large for its type, or one that isn't zero but would round to zero, is a compile error.

## Printing

Floats print exactly as Java's `Double.toString` and `Float.toString` print them (JDK 19 and later): the shortest decimal that reads back as the same value. Between 10⁻³ and 10⁷ it's written plainly, with at least one digit after the point. Outside that range it's written `d.dddE<n>`.

```calx
fn main() {
    let zero = 0.0
    println("{0.1 + 0.2} {100.0} {1.0e7} {0.001} {1.0e-4}")   // out: 0.30000000000000004 100.0 1.0E7 0.001 1.0E-4
    println("{-zero} {1.0 / zero} {zero / zero}")              // out: -0.0 Infinity NaN
    println("{1.0f32 / 3.0f32} {f64(0.1f32)}")                // out: 0.33333334 0.10000000149011612
}
```

The formatting is checked against Java by the rig. It's done in the runtime with the C library's correctly rounded conversions for now.

## Constants

`f32` and `f64` have `NAN`, `INFINITY`, `NEG_INFINITY`, `MAX` and `MIN_POSITIVE` (Java's `MIN_NORMAL`). Comparing anything with `NAN` is always false (or true for `!=`), so it's a compile error. Use `x.is_nan()`.

```calx
fn main() {
    let x = 1.0
    let _bad = x == f64.NAN    // error: comparing with NaN is always false; use `.is_nan()`
}
```

## Methods

| Method | Meaning |
| --- | --- |
| `x.abs()` | The absolute value |
| `x.min(y)`, `x.max(y)` | Java's `Math.min` and `Math.max`: NaN if either is NaN, and `-0.0` is less than `0.0`. Not C's `fmin`, which ignores NaN |
| `x.sqrt()` | The square root, correctly rounded |
| `x.floor()`, `x.ceil()` | Rounded down or up to an integer value, still a float |
| `x.mul_add(a, b)` | `x * a + b`, rounded once: IEEE 754's fused multiply-add and Java's `Math.fma`. It's exact everywhere, so every machine gives the same bits; with `x86-64-v3`, as in release builds, it's one instruction |
| `x.is_nan()` | Whether it's NaN |
| `x.total_cmp(y)` | Java's `Double.compare` and `Float.compare`, as an [`Ordering`](comparing.md#order): `-0.0` comes before `0.0`, and NaN after everything, equal to every NaN. Floats have no `Order`, since `<` can't order NaN, so this is how to sort by one |
| `x.to_bits()` | The IEEE 754 bits, a `u64` for `f64` and a `u32` for `f32`: Java's `doubleToRawLongBits` and `floatToRawIntBits`, which keep a NaN's payload |

```calx
fn main() {
    let neg_zero = -0.0
    let nan = f64.NAN
    println("{neg_zero.min(0.0)} {nan.max(1.0)} {2.0.sqrt()} {(-2.5).floor()}")   // out: -0.0 NaN 1.4142135623730951 -3.0
    println("{neg_zero.total_cmp(0.0) == Ordering.Less} {nan.total_cmp(f64.INFINITY) == Ordering.Greater}")   // out: true true
}
```

`f64.from_bits(b)` and `f32.from_bits(b)` make a float from its bits, as `longBitsToDouble` and `intBitsToFloat` do.

```calx
fn main() {
    println("{1.0.to_bits()} {(-0.0f32).to_bits()} {f64.from_bits(0x3fd0000000000000)}")    // out: 4607182418800017408 2147483648 0.25
}
```

`exp`, `log`, `pow`, the trigonometric functions and the rest are in [`std.math`](../std/math.md), which is fast, and [`std.java.math`](../std/java-math.md), which gives `StrictMath`'s results bit for bit. `round`, `rint`, `total_cmp` and `java_string` are designed in the [spec](../CALX_SPEC.md#transcendental-functions) and not built yet.

## Parsing

`f64.parse(text)` and `f32.parse(text)` read a float from a `str` or a `String`, giving an [optional](optionals.md). They take what Java's `Double.parseDouble` takes, and nothing more:

- Spaces and other characters up to `' '` around it are ignored.
- An optional sign, then `NaN`, `Infinity`, or a number: digits with an optional `.` and fraction (`1.`, `.5`), and an optional exponent (`e-3`), or a hex number with a binary exponent (`0x1.8p1`).
- An optional `f`, `F`, `d` or `D` at the end.

The result is correctly rounded. `f32.parse` rounds once, straight from the text, as `Float.parseFloat` does, so it can differ from parsing an `f64` and rounding that to `f32`.

```calx
fn main() {
    println("{f64.parse(" 1.5e3 ") ?? 0.0} {f64.parse("-Infinity") ?? 0.0} {f64.parse("0x1p-2") ?? 0.0}")    // out: 1500.0 -Infinity 0.25
    println("{f64.parse("inf") == none} {f64.parse("1e") == none}")    // out: true true
    println("{f32.parse("1.00000017881393432617187499") ?? 0.0}")    // out: 1.0000001
}
```

## Converting

Converting a float to an integer is Java's cast, `i32.saturate(f)`: it clamps to the range, and NaN gives 0. Converting to a float rounds with `f32.nearest(x)` unless every value converts exactly ([conversions](conversions.md)).
