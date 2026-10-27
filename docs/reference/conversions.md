---
title: Conversions
description: Converting between number types - exact conversions, the named lossy ones that match Java's casts, try_from, and enums.
---

# Conversions

No value changes type by itself in Calx. A conversion is always written, and its name says what it does (D63).

| Written | Does | Java |
| --- | --- | --- |
| `T(x)` | Converts exactly. It compiles only when every value of `x`'s type is exactly a `T`, so it never changes a value | Widening |
| `T.wrap(x)` | Keeps the low bits, between integer types | `(byte) x`, `(int) aLong`, reading an `int` as unsigned |
| `i32.saturate(f)`, `i64.saturate(f)` | Clamps a float to the type's range. NaN gives 0 | `(int) f`, `(long) d` |
| `i64.round(d)`, `i32.round(f)` | The nearest integer, with halves rounded up, clamped as `saturate` does. `i64.round` takes an `f64` and `i32.round` an `f32`, as Java's two do | `Math.round(d)`, `Math.round(f)` |
| `f32.nearest(x)`, `f64.nearest(x)` | Rounds to the nearest float, ties to even | `(float) d`, `(double) aLong`, `(float) anInt` |
| `T.try_from(x)` | `some(x)` as a `T` if it's one of `T`'s values, or `none` | A range check before a cast |

```calx
fn main() {
    let small: i32 = -5
    let big: i64 = 5000000000
    let d = 3.99
    println("{i64(small)} {f64(small)}")                     // out: -5 -5.0
    println("{i32.wrap(big)} {u8.wrap(small)} {i8.wrap(200)}") // out: 705032704 251 -56
    println("{i32.saturate(d)} {i32.saturate(-1.0e20)} {i32.saturate(f64.NAN)}")   // out: 3 -2147483648 0
    println("{i64.round(d)} {i64.round(-2.5)} {i32.round(0.49999997f32)}")   // out: 4 -2 0
    println("{f32.nearest(0.1)} {f64.nearest(9007199254740993i64)}")   // out: 0.1 9.007199254740992E15
}
```

## Which conversions are exact

- **Integer to integer:** when the source's whole range fits in the target's. `i32` to `i64`, `u8` to `i16`, `u32` to `u64`. Not `i32` to `u32`, since negative numbers don't fit.
- **Integer to float:** when every value is exactly representable: up to 24 bits for `f32` (`i16`, `u16`, `u8`, `i8`, `char`) and up to 53 bits for `f64` (`i32`, `u32` and smaller). Not `i64` to `f64`.
- **Float to float:** `f32` to `f64`.
- **Float to integer:** never. Use `saturate`, or `round`.
- **`char`** converts like a `u16`, and **`rune`** like a number from 0 to 0x10FFFF. Only a `u8` converts to a `rune` exactly, since other integers can be surrogates or too large.
- **`bool`** converts to and from nothing. Write `if b { 1 } else { 0 }`.

A conversion to the type a value already has is an error, and so is a named lossy conversion where the exact one would do. There's one right way to write each conversion.

```calx
fn main() {
    let n: i32 = 7
    let w: i64 = 9
    let _a = u32(n)        // error: `i32` to `u32` can lose information; keep the low bits with `u32.wrap(...)`
    let _b = i64(w)        // error: `w` is already an `i64`; remove the conversion
    let _c = i64.wrap(n)   // error: `i32` to `i64` can't lose information; write `i64(...)`
    let _d = i32(2.5)      // error: Java's cast is `i32.saturate(...)`
}
```

## Java's narrow casts

Java has no direct cast from a float to `byte`, `short` or `char`: `(byte) f` converts to `int` first, then narrows. Calx writes both steps, so the behavior is visible: `i8.wrap(i32.saturate(f))`. That's why only `i32.saturate` and `i64.saturate` exist.

```calx
fn main() {
    let f = 300.5
    println("{i8.wrap(i32.saturate(f))}")    // out: 44
}
```

## `usize`

`usize` is 64 bits on 64-bit targets and 32 bits on `wasm32`, so a conversion involving it must be exact on both:

- Into `usize`: exact from `u8`, `u16`, `u32` and `char`. Not from `u64`, which doesn't fit on `wasm32`, or from signed types.
- Out of `usize`: exact into `u64` only.

Indexes are `usize`. Java code indexes with `int`, which converts with `usize.wrap(i)`: a negative index becomes huge, and the bounds check catches it, as Java's would.

```calx
fn main() {
    let xs = [10, 20, 30]
    let i: i32 = 2
    println("{xs[usize.wrap(i)]}")    // out: 30
}
```

## `try_from`

`T.try_from(x)` converts an integer, `char` or `rune` to any of those types when the value fits, and gives `none` when it doesn't ([optionals](optionals.md)). It's the checked conversion, for input that might be out of range.

```calx
fn main() {
    println("{i8.try_from(127) ?? 0i8} {i8.try_from(128) == none} {u8.try_from(-1) == none}")   // out: 127 true true
    println("{char.try_from(65) ?? '?'} {rune.try_from(0xD800) == none}")                     // out: A true
}
```

A `rune` is never a surrogate, so `rune.try_from` rejects 0xD800 to 0xDFFF as well as values over 0x10FFFF.

## Enums

An enum with a [representation](enums.md#representations) converts to a number exactly as its representation would: `u8(flag)`, `i32(flag)`. `Flag.try_from(n)` goes the other way, taking the representation's type. An enum without a representation doesn't convert.
