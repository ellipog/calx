---
title: Integers
description: Integer arithmetic in Calx matches Java's bit for bit - overflow, division, shifts and literals.
---

# Integers

Calx integers behave exactly as Java's, so Calcite computes what vanilla Minecraft computes. Each rule here says what Calx does and how the emitted C does it, since every one of them is undefined or implementation-defined in C.

| Rule | Calx | Emitted C |
| --- | --- | --- |
| Overflow | `+`, `-`, `*` and unary `-` wrap (two's complement) for every integer type, as in Java | Arithmetic on the unsigned type of the same width, then conversion back. Also built with `-fwrapv` |
| Shifts | `<<`, `>>` and `>>>` work on `i32`, `i64`, `u32` and `u64`. A variable count is masked to 31 or 63, as in Java. `>>` is arithmetic on signed types, and `>>>` is logical | `x << (n & 31)` |
| `MIN / -1` | Gives `MIN`. `MIN % -1` gives `0` | A branch on `-1` before dividing, which traps on x86 otherwise |
| Division by zero | **Panics**, for `/` and `%` on every integer type (D55). Java throws `ArithmeticException`, which vanilla never catches, so the outcome is the same | A branch on zero, then the runtime's panic |
| Rounding | Division truncates toward zero, and the remainder takes the dividend's sign. `floor_div` and `floor_mod` are Java's `Math.floorDiv` and `Math.floorMod` | `/` and `%` after the checks |

```calx
fn main() {
    let max = i32.MAX
    let min = i32.MIN
    let minus_one = -1
    println("{max + 1} {min / minus_one} {min % minus_one}")   // out: -2147483648 -2147483648 0
    println("{-7 / 2} {-7 % 2} {(-7).floor_div(2)} {(-7).floor_mod(2)}")   // out: -3 -1 -4 1
    let byte: i8 = 127
    println("{byte + 1}")                       // out: -128
    let n = 33
    println("{1 << n} {-16 >> 2} {-16 >>> 28}")  // out: 2 -4 15
}
```

## Literals

- An integer literal takes its type from where it's used: `let x: u8 = 200`, `wide + 1` where `wide` is an `i64`, or an argument of type `u16`. When nothing gives it a type, it's an `i32`.
- A suffix fixes the type: `5i64`, `255u8`, `3usize`.
- A literal must fit its type, or it's a compile error. That includes hex: Java's `int` literal `0x9E3779B9` is a bit pattern that doesn't fit in an `i32`, so Calx writes it `i32.wrap(0x9E3779B9u32)`.
- A `usize` literal must fit in 32 bits, since `usize` is 32 bits on `wasm32`.
- `-128i8` is one literal, so the minimum of each type can be written directly.

```calx
fn main() {
    let a: u8 = 200
    let b: i64 = 1 << 40
    println("{a} {b} {-128i8} {i32.wrap(0x9E3779B9u32)}")   // out: 200 1099511627776 -128 -1640531527
}
```

```calx
fn main() {
    let _a: u8 = 256     // error: 256 doesn't fit in `u8` (0 to 255)
    let _b: i32 = 0xFFFFFFFF   // error: doesn't fit in `i32`
    let _c = -1u32       // error: `u32` can't be negative
}
```

## Constant expressions

An expression made only of literals and constants is computed while compiling. If it overflows its type, that's a compile error, even though the same operation wraps at run time: a constant that overflows is almost always a mistake. Write `wrapping_add`, `wrapping_sub` or `wrapping_mul` when wrapping is what you mean.

```calx
fn main() {
    let _a = i32.MAX + 1               // error: this constant expression overflows `i32`
    let b = i32.MAX.wrapping_add(1)
    println("{b}")
}
```

## Operators

- Both sides of `+`, `-`, `*`, `/`, `%`, `&`, `|` and `^` have the same type. There's no mixing an `i32` with an `i64` ([conversions](conversions.md)).
- A shift's count can be any integer type. A constant count must be from 0 to the width minus one, since Java masks `x << 32` on an `int` to a shift by 0, which is never what was meant.
- `i8`, `i16`, `u8` and `u16` don't shift. Java promotes them to `int` first, and Calx makes that explicit: `i32(b) << 2`.
- Unary `-` doesn't apply to unsigned types. `~` is bitwise not, for integers only.
- `>>>` on an unsigned type would be the same as `>>`, so it's an error there.

## Methods

| Method | Meaning |
| --- | --- |
| `x.abs()` | The absolute value, for signed types. `i32.MIN.abs()` is `i32.MIN`, as in Java |
| `x.min(y)`, `x.max(y)` | The smaller or larger |
| `x.wrapping_add(y)`, `wrapping_sub`, `wrapping_mul` | The same as `+`, `-` and `*`, which also wrap. In a constant expression they say that wrapping is meant |
| `x.floor_div(y)`, `x.floor_mod(y)` | Java's `Math.floorDiv` and `Math.floorMod`, for signed types. They panic on division by zero |

`checked_div`, `checked_add`, `saturating_add` and the like come with optionals.
