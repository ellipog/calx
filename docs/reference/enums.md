---
title: Enums
description: Sum types - variants with and without data, representations for wire formats, methods, printing and conversions.
---

# Enums

An enum is a value that is exactly one of its variants. A variant can carry data:

```calx
enum Shape {
    Circle(f64)
    Rect { w: f64, h: f64 }
    Empty
}

fn area(s: Shape) -> f64 {
    return match s {
        Circle(r) => 3.0 * r * r,
        Rect { w, h } => w * h,
        Empty => 0.0,
    }
}

fn main() {
    println("{area(Shape.Circle(2.0))} {area(Shape.Rect { w: 2.0, h: 3.0 })} {area(Shape.Empty)}")   // out: 12.0 6.0 0.0
}
```

## Declaring

`enum Name { ... }` lists the variants, one per line or separated by commas. Each variant is one of:

| Variant | Holds | Built with |
| --- | --- | --- |
| `Empty` | Nothing | `Shape.Empty` |
| `Circle(f64)` | Values by position | `Shape.Circle(2.0)` |
| `Rect { w: f64, h: f64 }` | Named fields | `Shape.Rect { w: 2.0, h: 3.0 }` |

- An enum needs at least one variant, and variant names are unique.
- A variant's fields are written in any order when building it, and their values run in the order written, as a [struct's](structs.md#building-a-value) do.
- `match` reads the data back out ([patterns](patterns.md)). There's no other way to reach it, so a variant's data can't be read as the wrong variant.
- An enum isn't a number. `Shape(1)` doesn't compile.

## Representations

An enum without data can name an integer type as its representation, and give its variants values. This is how protocol and save-format enums are written:

```calx
enum Flag: u8 { OnGround = 1, Sprinting = 4, Sneaking = 8 }

enum Level: i8 { Low = -1, Mid, High }

fn main() {
    println("{u8(Flag.Sprinting)} {u8(Flag.OnGround) | u8(Flag.Sneaking)}")   // out: 4 9
    println("{i8(Level.Low)} {i8(Level.Mid)} {i32(Level.High)}")               // out: -1 0 1
    println("{Flag.try_from(8u8) ?? Flag.OnGround} {Flag.try_from(2u8) == none}")   // out: Sneaking true
}
```

- A variant without a value takes the one before it plus one. The first takes 0.
- Values must fit the representation and be different from each other.
- `T(e)` converts an enum to a number as its representation would convert: exactly, so `u8(Flag.Sprinting)` and `i32(Flag.Sprinting)` both work, and `i8(Flag.Sprinting)` doesn't ([conversions](conversions.md)).
- `Flag.try_from(n)` goes the other way. `n` has the representation's type, and the result is a `Flag?`, which is `none` if no variant has that value ([optionals](optionals.md)).
- Only an enum with a representation converts. Without one, the values are the compiler's choice.

```calx
pub enum Valued { A = 1 }       // error: give the enum a representation, such as `enum Valued: u8`

pub enum Big: u8 { A = 255, B }  // error: the value of `B` would overflow `u8`

pub enum Carries: u8 { A(i32) }  // error: only an enum without data can have a representation

enum Color { Red, Green }

fn main() {
    let e = u8(Color.Red)       // error: `Color` has no number to convert; give it a representation
    println("{e} {Color.Green}")
}
```

## Comparing and printing

An enum without data compares with `==` and `!=`, and prints its variant's name. One with data does neither yet: compare it with `match`.

```calx
enum Color { Red, Green, Blue }

fn main() {
    let c = Color.Green
    println("{c} {c == Color.Green} {c != Color.Blue}")   // out: Green true true
}
```

## Methods

An `impl` block adds functions to an enum declared in the same module, as it does for a [struct](structs.md#methods). Inside it, `Self` means the enum.

```calx
enum Color { Red, Green, Blue }

impl Color {
    fn next(self) -> Self {
        return match self {
            Red => Color.Green,
            Green => Color.Blue,
            Blue => Color.Red,
        }
    }
}

fn main() {
    println("{Color.Red.next()} {Color.Blue.next()}")   // out: Green Red
}
```

## In memory

An enum without data is its representation, or the smallest of `u8`, `u16` and `u32` that numbers its variants. One with data is a tag of that size followed by the largest variant's fields, as a C tagged union. An enum whose variants hold only `Copy` values is `Copy`. One that holds a `String` or a `List` owns memory, and moves ([ownership](ownership.md)).
