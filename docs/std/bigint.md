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
- Multiplying by an `i64` or an `i32` is one pass into the product's limbs, with no `BigInt` made for the small one.
- Dividing two numbers of about the same size, as a spigot does for each digit, where `a` is at most a limb longer than `b` and the quotient fits in a limb, takes no division at all past the top: the quotient is guessed from the top limbs, as algorithm D's first step does, and one pass that writes nothing says whether it's one too big. On the Benchmarks Game's pidigits, 10,000 digits of π, that took Calx from 983 ms to 582, where Go's `math/big` takes 878 (CLX-298).
- Dividing is otherwise Knuth's algorithm D, which works out each limb of the quotient from the top two limbs of what's left with one [`div_wide`](../reference/integers.md#methods), a single `div` instruction on x86-64. From 64 limbs in the divisor, and as many more in the dividend, it's Burnikel and Ziegler's recursive division, which splits a division of 2n limbs by n into two of 3n/2 by n, each of them one of n by n/2 and a multiply. A division then costs what a multiply does times log n, where Knuth's costs the two lengths multiplied.
- Text is read and written 19 decimal digits at a time, the most that fit in a limb. From 30 limbs, about 570 digits, it's written by halves: the number is divided by 10^(19·2^k) with about half its limbs, and each half written the same way, with the powers worked out once. From 2,000 limbs' worth of digits, about 38,000, it's read by halves too, the upper half's value times the power, plus the lower half's. Below those sizes the plain loops are faster.
- The inner loops read and write the limbs through pointers, with the bounds checked once for the whole loop.
- Dividing by zero, with `/`, `%`, `div_rem`, `floor_div` or `floor_mod`, panics, naming the line that divides ([`@track_caller`](../reference/printing.md#panics-that-name-the-caller)).

On our benchmark, an i9-13900K pinned to its performance cores with other work running, so rough, the best of 20 runs, or of 3 from 5,000 limbs, against CPython 3.14 with no limit on the digits it converts. The dividend has 2n limbs and the divisor n, and the text is the dividend's:

| n limbs | Dividing, Calx | Python | Writing text, Calx | Python | Reading text, Calx | Python |
| --- | --- | --- | --- | --- | --- | --- |
| 50 | 2 µs | 10 µs | 9 µs | 37 µs | 3 µs | 10 µs |
| 200 | 23 µs | 107 µs | 84 µs | 596 µs | 28 µs | 127 µs |
| 1,000 | 0.35 ms | 1.39 ms | 1.16 ms | 3.93 ms | 0.52 ms | 1.80 ms |
| 5,000 | 4.75 ms | 16.6 ms | 16.3 ms | 41.4 ms | 8.71 ms | 23.0 ms |
| 20,000 | 44.7 ms | 146 ms | 152 ms | 211 ms | 81.3 ms | 214 ms |

At 20,000 limbs the text is 770,619 digits. Before the recursive division and the halves, the same numbers took 290 ms to divide, 2.76 s to write and 237 ms to read.
