---
title: Types
description: The primitive types, arrays, strings, lists, structs, enums, optionals, tuples, the unit type, and which types are Copy.
---

# Types

## Primitive types

| Type | Meaning |
| --- | --- |
| `i8`, `i16`, `i32`, `i64` | Signed integers, two's complement. `i8` is Java's `byte`, `i16` is `short`, `i32` is `int`, `i64` is `long` |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers, for wire formats and bit work |
| `usize` | An index or length. 64-bit on every platform Calx targets, and 32-bit on `wasm32`, so conversions treat it as either ([conversions](conversions.md#usize)) |
| `f32`, `f64` | IEEE 754 binary32 and binary64: Java's `float` and `double` |
| `bool` | `true` or `false` |
| `char` | An unsigned 16-bit UTF-16 code unit: Java's `char`. Not a number: convert it to do arithmetic |
| `rune` | A Unicode scalar value: what iterating a string will give |
| `str` | A read-only view of UTF-8 text. String literals are `str`, or `String` where one is wanted ([strings](strings.md)) |

Integers behave exactly as Java's do, including overflow, division and shifts ([integers](integers.md)). So do floats ([floats](floats.md)). No type converts to another by itself ([conversions](conversions.md)).

Each integer type has `MIN` and `MAX`, except that `usize.MAX` isn't available, since it depends on the target. The float types have `NAN`, `INFINITY`, `NEG_INFINITY`, `MAX` and `MIN_POSITIVE`.

```calx
fn main() {
    println("{i8.MIN} {i8.MAX} {u16.MAX} {i64.MAX}")    // out: -128 127 65535 9223372036854775807
    println("{f32.MAX} {f64.MIN_POSITIVE}")            // out: 3.4028235E38 2.2250738585072014E-308
}
```

## Arrays

`[T; N]` holds exactly `N` values of type `T`, where `N` is a constant `usize`. Arrays are values, stored where they're declared. An array of `Copy` elements is `Copy`, and one of elements that own memory moves ([ownership](ownership.md)).

```calx
const SIDE: usize = 3

fn main() {
    var grid: [i32; SIDE] = [0; SIDE]     // [x; n] is n copies of x
    grid[1] = 5
    let primes = [2, 3, 5, 7]             // the length comes from the literal
    var sum = 0
    for p in primes {
        sum += p
    }
    println("{grid[1]} {sum}")            // out: 5 17
}
```

- `a[i]` reads element `i`, where `i` is a `usize`. An index out of bounds panics. A constant index out of bounds is a compile error.
- `for x in a` goes over the elements in order ([control flow](control-flow.md#for)).
- An empty `[]` needs its type from where it's used.

## Strings and lists

- `String` owns UTF-8 text and can change it ([strings](strings.md)).
- `List<T>` is a growable array that owns its elements ([lists](lists.md)).
- `Deque<T>` is a growable ring, added to and taken from at both ends ([deques](deques.md)).

## Structs, enums and tuples

- A struct holds named fields ([structs](structs.md)).
- An enum is one of several variants, each of which can carry data ([enums](enums.md)).
- A tuple, such as `(i32, str)`, groups values by position ([tuples](tuples.md)).
- Structs and enums can be generic, as in `Pair<i32, str>` ([generics](generics.md)).
- An interface isn't a type. It's a bound on a generic parameter ([interfaces](interfaces.md)).

## Pointers

`*T` and `*const T` are raw pointers, for [unsafe code](unsafe.md#pointers) and C functions only.

## Optionals

`T?` is a `T` that may be absent: `some(x)` or `none`. There's no null ([optionals](optionals.md)).

## `Copy` and owning types

A type is `Copy` when its values own no memory: the primitive types, enums without data, and arrays, tuples, optionals, structs and enums made only of `Copy` types. Assigning or passing one copies it. `String`, `List` and the types that hold them own memory, so they move instead, and are freed when their owner ends ([ownership](ownership.md)).

## The unit type

`()` is the type of a function with no `->`, and of statements. It has one value, also written `()`. A variable or parameter can't be `()`.
