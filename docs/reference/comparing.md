---
title: Comparing and printing
description: The derived Equal and Display, which give == and {x} to structs, enums, tuples, arrays, optionals and lists; the bounds T: Equal and T: Display; and their C.
---

# Comparing and printing

A value can be compared with `==` and shown in a string whenever everything it holds can. These are two of the derived properties ([spec](../CALX_SPEC.md#derived-properties)): the compiler works them out for each type, so there's nothing to write.

```calx
struct Point {
    x: i32,
    y: i32,
}

enum Shape {
    Dot(Point)
    Line(Point, Point)
    Rect { w: f64, h: f64 }
}

fn main() {
    let a = Shape.Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })
    println("{a}")    // out: Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })
    println("{a == Shape.Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })}")    // out: true
    println("{a != Shape.Rect { w: 1.0, h: 1.0 }}")    // out: true
    let p = Point { x: 3, y: 4 }
    let where = "at {p}"
    println(where)    // out: at Point { x: 3, y: 4 }
}
```

## `Equal`

`a == b` is true when `a` and `b` hold equal values, and `a != b` when they don't. Both sides have the same type.

| Type | Equal when |
| --- | --- |
| A struct | Every field is equal |
| An enum | Both are the same variant, and its values are equal |
| A tuple or an array | Every part is equal |
| An optional | Both are `none`, or both hold equal values ([optionals](optionals.md#comparing)) |
| `List<T>` | Both have the same length, and equal elements in the same order |
| `String` and `str` | Both hold the same bytes |
| A float | As floats compare: `NaN` never equals anything, and `0.0 == -0.0` ([floats](floats.md)) |

The parts are compared in order, and the comparison stops at the first difference. Every field counts, private ones too.

Every type has `Equal` except a generic parameter without the bound, and what holds one. `<`, `<=`, `>` and `>=` are for numbers, `char` and `rune` only, until `Order` is built.

```calx
enum Item {
    Stone
    Sword(i32)
}

fn main() {
    let a: List<Item> = [Item.Stone, Item.Sword(5)]
    var b: List<Item> = [Item.Stone]
    println("{a == b}")    // out: false
    b.push(Item.Sword(5))
    println("{a == b} {b.contains(Item.Sword(5))}")    // out: true true
    let nan = 0.0 / 0.0
    println("{(1, nan) == (1, nan)}")    // out: false
}
```

## `Display`

`{x}` in a string shows `x` the way code builds it:

| Type | Shows as |
| --- | --- |
| A struct | Its literal: `Point { x: 1, y: 2 }`, or `Empty {}` with no fields. A generic struct's name has no arguments: `Pair { first: 1, second: one }` |
| An enum | The variant, as it's built but without the enum's name: `Stone`, `Sword(5)`, `Rect { w: 2.0, h: 3.0 }` |
| A tuple | `(1, a, true)` |
| An array or a `List` | `[1, 2, 3]`, as Java's `ArrayList.toString` ([lists](lists.md#printing)) |
| An optional | `some(5)` or `none` |
| `String`, `str`, `char`, `rune` | The text itself, without quotes, even inside a struct or a list |
| A number or a `bool` | As Java prints it ([integers](integers.md), [floats](floats.md#printing)) |

Every field is shown, private ones too, so `Display` is for logs and debugging. Every type has it except a pointer, and a generic parameter without the bound, and what holds either.

```calx
struct Player {
    name: String,
    health: f32,
    friends: List<String>,
    title: String?,
}

fn main() {
    let p = Player { name: String("Steve"), health: 20.0, friends: [String("Alex")], title: none }
    println("{p}")    // out: Player { name: Steve, health: 20.0, friends: [Alex], title: none }
}
```

## In generic code

Generic code compares and prints a `T` only with the bounds `T: Equal` and `T: Display` ([generics](generics.md#bounds)), and any type that has the property can be the argument:

```calx
fn index_of<T: Equal>(xs: List<T>, x: T) -> usize? {
    for i in 0usize..xs.len() {
        if xs[i] == x {
            return some(i)
        }
    }
    return none
}

fn labelled<T: Display>(label: str, x: T) -> String {
    return "{label}: {x}"
}

fn main() {
    let words: List<String> = [String("a"), String("b")]
    println("{index_of(words, String("b"))} {index_of(words, String("c"))}")    // out: some(1) none
    println(labelled("at", (1, 2)))    // out: at: (1, 2)
}
```

Without the bound, the error says which to add:

```calx
fn same<T>(a: T, b: T) -> bool {
    return a == b    // error: values of type `T` can't be compared with `==`; give `T` the bound `Equal`
}

fn main() {
    println("{same(1, 1)}")
}
```

Like `Copy`, `Equal` and `Display` are properties, not interfaces: no `impl` gives them, and the names are reserved. A type writing its own, to compare only some fields or to show itself another way, is designed but not built ([spec](../CALX_SPEC.md#derived-properties)).

## In C

A file that compares or prints a struct, an enum with data, a tuple, an array, an optional or a list gets a `static inline` function for it, and for each such type it holds: `cx_eq_<type>(a, b)` and `cx_show_<type>(v)`. Only the ones the file uses are made.

`cx_eq_` is the field comparisons, in order, joined with `&&`, so comparing two `Point`s is two integer comparisons once clang inlines it. `cx_show_` writes each piece straight to the output, or to the `String` being built, so showing a value builds no strings for its parts, and the text between two values is written in one go.
