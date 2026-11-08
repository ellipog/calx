---
title: std.bigint
description: Integers of any size, as Python's int and Java's BigInteger are, with the arithmetic operators, powers, roots, gcd, and text in any base.
---

# `std.bigint`

`BigInt` is an integer of any size, for numbers that don't fit in an `i64`: factorials, exact fractions' parts, cryptography exercises, and IDs and checksums wider than 64 bits. It's what Python's `int` is past 64 bits, and Java's `BigInteger`.

```calx
import std.bigint.{BigInt}

fn main() {
    var f = BigInt.from(1)
    for i in 1..=25 {
        f *= i
    }
    println("{f}")                                    // out: 15511210043330985984000000
    let big = BigInt.parse("123456789012345678901234567890")!
    println("{big * big / f}")                        // out: 982617004776614660550646961471554
    println("{BigInt.from(2).pow(100) - 1}")          // out: 1267650600228229401496703205375
    println("{big.to_radix(16)} {big < f} {big.is_even()}")  // out: 18ee90ff6c373e0ee4e3f0ad2 false true
}
```

## Making one

| Item | Gives |
| --- | --- |
| `BigInt.from(n)` | `n`, an `i32`, an `i64` or a `u64` |
| `BigInt.zero()` | 0 |
| `BigInt.parse(text)` | The integer in decimal text, an optional `+` or `-` and then digits, as [`i64.parse`](../reference/integers.md#parsing) takes them, or `none` |
| `BigInt.parse_radix(text, radix)` | The same in base `radix`, from 2 to 36, with letters after 9 in either case |
| `BigInt.from_bytes_be(bytes)` | The integer whose bytes, the most significant first, are `bytes`, as Python's `int.from_bytes(b, "big")`. It's never negative |

## Arithmetic

`+`, `-`, `*`, `/`, `%` and a prefix `-` work on `BigInt`s, through the [operator interfaces](../reference/interfaces.md#operators), and so do `+=` and the others. The right side can be a `BigInt`, an `i64` or an `i32`, so `n * 2`, `n + i` in a loop over `i32`s, and `a * b` all work. Each makes a new `BigInt` and only reads its operands, so they can be used again.

- `/` and `%` round toward zero, as they do on every integer type in Calx, and Java's `BigInteger` does. `floor_div` and `floor_mod` round down, as Python's `//` and `%` do. Dividing by zero panics.
- `==`, `<` and the other comparisons work between `BigInt`s, as do `sort` and the keys of maps and sets. A `BigInt` doesn't compare with an `i32`; `n.is_zero()` and `n.sign()` are the usual tests, or `n == BigInt.from(5)`.

| Method | Gives |
| --- | --- |
| `n.div_rem(d)` | The quotient and the remainder together, rounded toward zero, in one division |
| `n.floor_div(d)`, `n.floor_mod(d)` | Python's `n // d` and `n % d`: the remainder has `d`'s sign |
| `n.pow(e)` | `n` to the power `e`, a `u32` |
| `n.mod_pow(e, m)` | `n` to the power `e`, modulo `m`, without the whole power, as Python's `pow(n, e, m)`. It's from 0 up to `m` |
| `n.gcd(m)` | The greatest common divisor, never negative |
| `n.isqrt()` | The largest integer whose square isn't more than `n`, as Python's `math.isqrt` |
| `n.abs()`, `n.sign()` | The absolute value, and -1, 0 or 1 |
| `n.shl(k)`, `n.shr(k)` | `n` times 2^k, and divided by 2^k rounded down, as `<<` and `>>` are on a Python `int` |
| `n.is_zero()`, `n.is_negative()`, `n.is_even()` | What they say |
| `n.bit_len()` | The bits the magnitude needs, as Python's `bit_length`: 0 for 0, 8 for 255 and -255 |

## Converting

| Method | Gives |
| --- | --- |
| `"{n}"` | Decimal digits, with a `-` before a negative number |
| `n.to_radix(radix)` | Digits in base `radix`, from 2 to 36, lowercase: `n.to_radix(16)` is Python's `format(n, "x")` |
| `n.to_i64()`, `n.to_u64()` | `some` of the value, or `none` when it doesn't fit |
| `n.to_f64()` | The nearest `f64`, ties to even, as Python's `float(n)`, or infinity past the largest |
| `n.div_f64(d)` | The `f64` nearest `n / d`, ties to even, as Python's `/` on two `int`s, however big: `BigInt.from(1).div_f64(BigInt.from(3))` is `0.3333333333333333`. Past the largest `f64` it's infinity, where Python raises `OverflowError` |
| `n.to_bytes_be()` | The magnitude's bytes, the most significant first, with no zero bytes before them. Zero has none |

## How it works

A `BigInt` is a sign and a list of 64-bit limbs, the lowest first, with no zero limb at the top, so each number has one form. Python's `int` has 30-bit digits, so each step of Calx's arithmetic does about four times as much.

- Multiplying takes each limb's whole 128-bit product, from [`widening_mul`](../reference/integers.md#methods). From 32 limbs in each factor, about 600 decimal digits, it's Karatsuba's method, which makes three products of half the size instead of four, as Python and Java do.
- Dividing is Knuth's algorithm D, which works out each limb of the quotient from the top two limbs of what's left with one [`div_wide`](../reference/integers.md#methods), a single `div` instruction on x86-64. Python 3.12 and later switch to a recursive method for numbers of tens of thousands of digits, which is faster at those sizes.
- Decimal text is read and written 19 digits at a time, the most that fit in a limb.
- The inner loops read and write the limbs through pointers, with the bounds checked once for the whole loop.
- A panic in an operator, such as dividing by zero with `/`, names a line of `std.bigint`, since an operator's function can't be `@track_caller` yet. `div_rem`, `floor_div` and `floor_mod` name the caller's line.
