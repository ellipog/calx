---
title: Comparing and printing
description: The derived Equal, Order and Display, which give ==, < and {x} to structs, enums, tuples, arrays, optionals, lists, maps and sets; Ordering and cmp; the bounds T: Equal, T: Order and T: Display; and their C.
---

# Comparing and printing

A value can be compared with `==`, ordered with `<` and shown in a string whenever everything it holds can. These are three of the derived properties ([spec](../CALX_SPEC.md#derived-properties)): the compiler works them out for each type, so there's nothing to write. A fourth, [`Hash`](maps.md#hash), says which types can be the keys of a map.

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
| `Map<K, V>`, `Set<T>` | Both have the same keys, with equal values, in any order ([maps and sets](maps.md#comparing-and-printing)) |
| `String` and `str` | Both hold the same bytes |
| A float | As floats compare: `NaN` never equals anything, and `0.0 == -0.0` ([floats](floats.md)) |

The parts are compared in order, and the comparison stops at the first difference. Every field counts, private ones too.

Every type has `Equal` except a generic parameter without the bound, and what holds one.

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

## `Order`

`a < b`, `a <= b`, `a > b` and `a >= b` compare two values of the same type in order, and `a.cmp(b)` says which comes first: an `Ordering`, which is `Less`, `Equal` or `Greater`. [`xs.sort()`](lists.md#sorting) puts a list in this order.

| Type | Ordered by |
| --- | --- |
| Integers, `char`, `rune` | Value |
| `bool` | `false` before `true` |
| `String` and `str` | Byte by byte, and a string that runs out first comes first. For UTF-8 that's the order of the code points |
| A struct | Its fields in the order they're declared: the first that differs decides |
| An enum | Its variants in the order they're declared, whatever their values; then the variant's values in order |
| A tuple or an array | Its parts in order |
| `List<T>` | Its elements in order, and a list that runs out first comes first |
| An optional | `none` before any value, as Java's `Comparator.nullsFirst` |
| `Ordering` | `Less`, `Equal`, `Greater` |

Floats have no `Order`, nor do pointers, maps and sets, or what holds them. `<` on two floats compares them as IEEE does, but `NaN` is neither less nor greater than anything, which would leave a sort's result undefined, so a struct holding a float can't be compared with `<`. To order by a float, use [`total_cmp`](floats.md#methods), Java's `Double.compare`, which puts `-0.0` before `0.0` and every `NaN` last.

Java's `String.compareTo` compares UTF-16 code units instead. The two orders differ only where one string has a character above U+FFFF, such as an emoji, and the other has one from U+E000 to U+FFFF in the same place.

```calx
struct Version {
    major: i32,
    minor: i32,
}

enum Rank {
    Guest
    Member
    Admin
}

fn name(o: Ordering) -> str {
    return match o {
        Less => "less",
        Equal => "equal",
        Greater => "greater",
    }
}

fn main() {
    let a = Version { major: 1, minor: 20 }
    let b = Version { major: 2, minor: 0 }
    println("{a < b} {name(a.cmp(b))} {name(b.cmp(b))}")    // out: true less equal
    println("{Rank.Admin > Rank.Member} {"apple" < "apples"}")    // out: true true
    var ranks: List<Rank> = [Rank.Admin, Rank.Guest, Rank.Member]
    ranks.sort()
    println("{ranks}")    // out: [Guest, Member, Admin]
}
```

What lacks `Order` says why:

```calx
struct Mob {
    name: String,
    health: f32,
}

fn main() {
    let a = Mob { name: String("zombie"), health: 20.0 }
    println("{a < a}")    // error: values of type `Mob` can't be compared with `<`, since it holds `f32`, and floats have no `Order`, since `NaN` is neither less than nor greater than anything; compare them with `total_cmp`
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
| A `Map` or a `Set` | `{1: one, 2: two}` or `{1, 2}`, in the order a loop goes ([maps and sets](maps.md#order)) |
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

Generic code compares, orders and prints a `T` only with the bounds `T: Equal`, `T: Order` and `T: Display` ([generics](generics.md#bounds)), and any type that has the property can be the argument. `T: Order` gives `==` too.

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

fn largest<T: Order>(xs: List<T>) -> usize {
    var best = 0usize
    for i in 1usize..xs.len() {
        if xs[i] > xs[best] {
            best = i
        }
    }
    return best
}

fn main() {
    let words: List<String> = [String("a"), String("b")]
    println("{index_of(words, String("b"))} {index_of(words, String("c"))}")    // out: some(1) none
    println(labelled("at", (1, 2)))    // out: at: (1, 2)
    println("{largest(words)}")    // out: 1
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

Like `Copy` and `Hash`, `Equal`, `Order` and `Display` are properties, not interfaces: no `impl` gives them, and the names are reserved, as is `Ordering`. A type writing its own, to compare only some fields or to show itself another way, is designed but not built ([spec](../CALX_SPEC.md#derived-properties)).

## In C

A file that compares or prints a struct, an enum with data, a tuple, an array, an optional, a list or a table gets a `static inline` function for it, and for each such type it holds: `cx_eq_<type>(a, b)` and `cx_show_<type>(v)`. A key type gets `cx_hash_<type>(v)` the same way. Only the ones the file uses are made.

`cx_eq_` is the field comparisons, in order, joined with `&&`, so comparing two `Point`s is two integer comparisons once clang inlines it. `cx_show_` writes each piece straight to the output, or to the `String` being built, so showing a value builds no strings for its parts, and the text between two values is written in one go.

Ordering one of those types, or an enum whose values don't go up in the order its variants are declared, takes `cx_cmp_<type>(a, b)`, which gives -1, 0 or 1: the parts compared in turn until one differs. `a.cmp(b)` adds 1, so `Less`, `Equal` and `Greater` are 0, 1 and 2. Strings compare with `cx_str_cmp`, which compares the first 8 bytes as one big-endian integer, since most strings differ there, before calling `memcmp` on the rest.
