---
title: std.decimal
description: Decimal numbers of any precision, as Python's decimal module has them, with exact money arithmetic, every rounding, and correctly rounded square roots, exponentials, logarithms and powers.
---

# `std.decimal`

`Decimal` is a decimal number of any size and precision. It's what Python's `decimal` module is for, and Java's `BigDecimal`:

- **Money:** `0.1 + 0.2` is `0.3`, and cents are kept as written.
- **Constants and tables:** work to as many digits as they need.
- **Rounding:** numbers are rounded the way a contract or a tax form says.

Every result is the one Python's `decimal` gives in the same context. [`tools/decimal/check.calx`](../../tools/decimal/check.calx) checks that on random values against CPython.

```calx
import std.decimal.{Context, Decimal, Rounding}

fn main() {
    let price = Decimal.parse(String("19.99"))!
    let tax = Decimal.parse(String("0.0825"))!
    let total = price * 3 * (tax + 1)
    println("{total} {total.round(2)}")                     // out: 64.917525 64.92
    println("{Decimal.from(1) / Decimal.from(3)}")          // out: 0.3333333333333333333333333333
    let c = Context.new(50)
    println("{c.from(2).sqrt()}")                           // out: 1.4142135623730950488016887242096980785696718753769
    let cash = total.with_rounding(Rounding.Down).round(1)
    println("{cash} {cash.to_plain()} {Decimal.parse(String("1.5E+3"))!}")   // out: 64.9 64.9 1.5E+3
}
```

## Making one

| Item | Gives |
| --- | --- |
| `Decimal.parse(text)` | The number in `text`, or `none`. See [Text](#text) |
| `Decimal.from(n)` | `n`, an `i32` or an `i64` |
| `Decimal.from_bigint(n)` | `n`, a [`BigInt`](bigint.md) |
| `Decimal.from_parts(c, e)` | `c × 10^e` exactly, `c` a `BigInt`: `Decimal.from_parts(BigInt.from(-15), -1)` is -1.5 |
| `Decimal.from_f64(x)` | The float's exact value, as Python's `Decimal(x)`: `0.1` is `0.1000000000000000055511151231257827021181583404541015625`. `none` for NaN and the infinities |
| `Decimal.zero()` | 0 |

Each makes a number in the default context. The same functions on a `Context` make one in that context: `c.parse(text)`, `c.from(n)`, `c.from_bigint(n)` and `c.from_f64(x)`. None of them rounds: a number keeps every digit it's made with until arithmetic rounds a result.

## Contexts

A context is a precision, in significant digits, and a [rounding](#rounding). Python keeps one for each thread, which `getcontext().prec = 50` changes. In Calx each number carries its own, so a script working at 80 digits makes its numbers in an 80-digit context and writes plain arithmetic after that:

```calx
import std.decimal.{Context}

fn main() {
    let c = Context.new(80)
    let ln2 = c.from(2).ln()
    let x = ln2 / 128 * 3 + c.from(1)
    println("{x}")    // out: 1.0162456370443737181894663778466760133142695343990684825184534377225014130149147
}
```

- **The default:** `Context.default()` is Python's default, 28 digits with ties to even. `Context.new(prec)` is `prec` digits with ties to even, and `c.with_rounding(r)` changes the rounding. `c.prec()` and `c.rounding()` read them.
- **Two operands:** a result from two numbers takes the context with more digits, or the left one's when they have the same. So a 28-digit number made with `Decimal.parse` takes on 80 digits once it meets an 80-digit one.
- **Integers:** a `Decimal` with an integer on the right, as in `x / 128`, uses `x`'s context.
- **Changing it:** `x.with_context(c)`, `x.with_prec(p)` and `x.with_rounding(r)` give the same digits in another context, and `x.context()` reads it.

## Arithmetic

`+`, `-`, `*`, `/`, `%` and a prefix `-` work through the [operator interfaces](../reference/interfaces.md#operators), and so do `+=` and the others. The right side can be a `Decimal`, an `i64` or an `i32`. Each result is rounded to the context, as Python rounds it:

- `+`, `-` and `*` are exact until a result has more digits than the precision. So money adds up exactly: `19.99 * 3` is `59.97`.
- `/` gives the exact quotient when it fits, at the exponent nearest the difference of the two exponents: `6.0 / 2` is `3.0`, and `1 / 8` is `0.125`. Otherwise it gives the quotient rounded to the precision. Dividing by zero panics, where Python raises `DivisionByZero`.
- `%` is Python's: the remainder of the quotient rounded toward zero, with the sign of the left side. `-7 % 3` is `-1`.
- `-x` and `x.abs()` round to the context too, as Python's do.
- Zeros have signs, as in Python: `-0 + -0` is `-0`, `-1 + 1` is `0`, and `0.00 * -5` is `-0.00`. The context's rounding matters here: `-1 + 1` is `-0` when it rounds toward negative infinity.

Comparing is by value, so `1.50 == 1.5` and `-0 == 0`. That holds for `<`, `sort`, and the keys of maps and sets, where `1.5` and `1.50` are one key. `x.min(y)` and `x.max(y)` give the smaller and the larger, or `x` when they're equal, as Python's `min` and `max` do.

| Method | Gives |
| --- | --- |
| `x.sqrt()` | The square root. It panics below 0 |
| `x.exp()` | e to the power `x`. It panics for `x` of 10^18 or more, or -10^18 or less |
| `x.ln()`, `x.log10()` | The natural and the base 10 logarithm. They panic for 0 and below |
| `x.powi(n)` | `x` to the power of an `i64`. It panics for 0 to a power of 0 or less |
| `x.pow(y)` | `x` to the power `y`, a `Decimal`, as Python's `x ** y`: `powi`'s for an integer `y`. It panics for a negative `x` to a power that isn't an integer, and for 0 to a power of 0 or less |

They're correctly rounded: the result is the exact value rounded once. Python promises that for `exp`, `ln`, `log10` and `sqrt`, which round ties to even whatever the context's rounding says. Calx does the same, so those four match Python digit for digit.

`powi` and `pow` round with the context's rounding, once. Python's `**` is only almost always correctly rounded, so where its last digit is off, Calx's differs from it:

- **Integer powers:** libmpdec multiplies at a few more digits and rounds twice. `6652653121749245896E-24 ** -1581` rounded down to one digit is `6E+8184`, where Python gives `7E+8184`.
- **Other powers:** libmpdec works them out from `exp` and `ln`, and in the directed roundings it can miss an exact result. 4 to the power 1.5 rounded down is `8.000000000000000000000000000`, where Python gives `7.999999999999999999999999999`.

A power that isn't an integer has the precision's digits even when it's exact, as Python writes it: 4 to the power 0.5 is `2.000000000000000000000000000`.

An exact result is kept exact, as Python keeps it:

- The square root of `4.00` is `2.0`, at half the exponent.
- `log10(1000)` is `3`.
- `1.5` to the power 2 is `2.25`.
- `10` to the power 1000 is `1.000000000000000000000000000E+1000`, the exact value rounded to 28 digits.

## Rounding

| `Rounding` | Rounds | Python |
| --- | --- | --- |
| `HalfEven` | To the nearest, ties to an even last digit. The default | `ROUND_HALF_EVEN` |
| `HalfUp` | To the nearest, ties away from zero, as schools teach | `ROUND_HALF_UP` |
| `HalfDown` | To the nearest, ties toward zero | `ROUND_HALF_DOWN` |
| `Up` | Away from zero | `ROUND_UP` |
| `Down` | Toward zero | `ROUND_DOWN` |
| `Ceiling` | Toward positive infinity | `ROUND_CEILING` |
| `Floor` | Toward negative infinity | `ROUND_FLOOR` |
| `ZeroFiveUp` | Toward zero, unless the last digit would be 0 or 5, then away | `ROUND_05UP` |

These methods round a number to a place, with its context's rounding:

| Method | Gives |
| --- | --- |
| `x.round(places)` | `x` rounded to `places` digits after the point, as Python's `round(x, places)`: `2.675` to 2 places is `2.68`, since a decimal holds it exactly. A negative `places` rounds to tens and up |
| `x.quantize(y)` | `x` at `y`'s exponent, as Python's `quantize`: `x.quantize(Decimal.parse("0.01")!)` gives cents |
| `x.to_integral()` | `x` rounded to an integer, as `to_integral_value`. A number with no digits after the point stays as it is, so `1E+5` stays `1E+5` |
| `x.normalize()` | `x` rounded to the context, without the zeros at the end: `1.500` is `1.5`, `1200` is `1.2E+3`, and `0.00` is `0` |

`round` and `quantize` panic when the result needs more digits than the precision, as Python's raise `InvalidOperation`. For another rounding, change it first: `price.with_rounding(Rounding.HalfUp).round(2)`.

## Text

`"{x}"` writes a number as Python's `str` does, which is the General Decimal Arithmetic's to-scientific-string:

- **Plain:** digits with a point when the exponent is 0 or below and the number isn't tiny. Examples are `12.50`, `0.000001` and `-0.00`.
- **Scientific:** an exponent when the exponent is above 0, or the number is below 10^-6. Examples are `1.23E+5`, `1E-7` and `1E+1`.

`x.to_plain()` writes the digits with no exponent, as Python's `format(x, "f")`: `1.5E+3` is `1500`.

`Decimal.parse` reads what Python's `Decimal(text)` reads:

- Spaces around the number, and an optional `+` or `-`.
- Digits with an optional point: `5.` and `.5`.
- An optional exponent, `e` or `E` with an optional sign: `1.5e-3`.
- `_` between two digits groups them: `1_000.000_1`. Python takes a `_` anywhere, as in `_1` and `1__0`. Calx takes one only between two digits, as Python's own literals do.

## Converting

| Method | Gives |
| --- | --- |
| `x.to_f64()` | The nearest `f64`, ties to even, as Python's `float(x)`: infinity past the largest double, and 0 below the least |
| `x.to_bigint()` | The integer part, rounded toward zero, as Python's `int(x)` |
| `x.to_i64()` | The same as an `i64`, or `none` when it doesn't fit |
| `x.coefficient()`, `x.exponent()` | The coefficient, never negative, and the exponent: `1.50` is 150 and -2 |
| `x.digits()`, `x.adjusted()` | The digits in the coefficient, and the exponent of the first one, as Python's `adjusted`: 2 for 123.4 |
| `x.sign()`, `x.is_zero()`, `x.is_negative()`, `x.is_signed()` | -1, 0 or 1; whether it's 0; whether it's below 0; and whether its sign is negative, which `-0`'s is |

## Unlike Python

- There's no NaN or infinity. The operations that would make one panic instead, and `parse` gives `none` for their text.
- There are no signals or traps. What Python raises by default is a panic, and what it only flags, such as `Inexact`, isn't kept.
- The exponent goes as far as libmpdec's largest context allows, ±999999999999999999, with no subnormal numbers below that. Python's default context stops at ±999999. A result past that panics, where Python's underflows to zero: `0.5 ** 1E+30`.

## How it works

A `Decimal` is a sign, a coefficient and an exponent, with the context.

- **The coefficient:** a list of limbs of nine decimal digits each, base 10^9, the lowest first, as libmpdec keeps its in base 10^19. Rounding to a number of digits cuts limbs and divides the rest by a power of ten, each limb with a multiply and a shift rather than a division, and writing the digits out needs no change of base.
- **Multiplying:** a column of the product at a time, with the column's products summed in 64 bits and carried every 16 products.
- **Dividing:** Knuth's algorithm D. Up to 36 digits in the divisor, it works in the coefficient's limbs, and each quotient limb is guessed with a multiply by the divisor's reciprocal. From 37, it puts the limbs together two at a time, into limbs of 18 digits, a quarter of the steps, as libmpdec works in limbs of 19. Each product of a quotient limb and a divisor limb, up to 36 digits, is split at 10^18 with a multiply by that divisor limb's 2^64 / 10^18, worked out once a division, and the borrows that pass from one limb to the next come from the top bits of differences, with no branches.
- **Square root:** Newton's method on the coefficient. It starts from a double, or from the root of the top half of the digits, which has half the digits right, so one step gets them all.
- **exp, ln and log10:** worked out in binary fixed point on `BigInt`s, to a few more digits than the precision, with a bound on the error. When the result rounds the same at both ends of the bound, that's the correctly rounded result; otherwise they're worked out again with more digits, which is Ziv's method. ln 2 and ln 10 come from a table to 2048 bits, and from their series past that. `exp` reduces by ln 10 and by halving, then squares back. `ln` takes out powers of ten and of two, so what's left is within √2 of 1, and sums the series of atanh.
- **powi:** an exact result is worked out exactly. Otherwise the power lies between two bounds, cut down and up to a few more digits than the precision, until both round the same.
- **pow:** x^y has a finite number of digits only when, with x = c × 10^e and y = a / b in lowest terms, b divides the powers of 2 and 5 in c × 10^e and the rest of c is a perfect b-th power. Such a result is worked out exactly. Every other one is e^(y ln x), with Ziv's method, which ends because a value that isn't exact is never where the rounding changes.

On our benchmark, the time for each operation at 28, 80 and 300 digits, against Python 3.14's `decimal`, which is libmpdec in C. The times are in nanoseconds, for 1.4142… against 1.7320…, the best of 15 runs taking turns, on the performance cores of an i9-13900K on Windows.

| Operation | Calx 28 | Python 28 | Calx 80 | Python 80 | Calx 300 | Python 300 |
| --- | --- | --- | --- | --- | --- | --- |
| `+` | 15 | 71 | 19 | 80 | 33 | 85 |
| `*` | 30 | 82 | 71 | 176 | 519 | 813 |
| `/` | 98 | 139 | 176 | 285 | 763 | 1407 |
| `sqrt` | 482 | 860 | 1043 | 2725 | 3727 | 19136 |
| `exp` | 1898 | 3375 | 3285 | 17587 | 13370 | 534347 |
| `ln` | 3391 | 9484 | 6255 | 35754 | 31250 | 650288 |

Python's times include its interpreter's overhead, tens of nanoseconds an operation. Dividing at 1000 digits takes 5.3 µs to Python's 11.6, and at 4000, 69 µs to 406. [`tools/math_tables.calx`](../../tools/math_tables.calx), which fits std.math's polynomials at 80 digits, takes 0.92 s where the Python it was ported from took 3.3 s.
