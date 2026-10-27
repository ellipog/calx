---
title: Optionals
description: T? instead of null - some and none, ??, !, ?., comparing, if let and let-else.
---

# Optionals (D23)

There's no null. A value that may be absent is a `T?`, which is either `none` or `some(x)`:

```calx
fn half(n: i32) -> i32? {
    if n % 2 == 0 {
        return some(n / 2)
    }
    return none
}

fn quarter(n: i32) -> i32 {
    let h = half(n) else return -1    // unwrap, or leave
    return half(h) ?? -2              // unwrap, or use a default
}

fn main() {
    if let h = half(12) {             // unwrap for one block
        println("half is {h}")        // out: half is 6
    }
    println("{quarter(8)} {quarter(6)} {quarter(5)}")   // out: 2 -2 -1
}
```

## Making one

- `some(x)` holds `x`. `none` holds nothing, and takes its type from where it's used: `let n: i32? = none`.
- A `T` never turns into a `T?` by itself, so `return n` in a function returning `i32?` doesn't compile. Write `return some(n)` (D63).
- An optional can't hold another optional, or `()`. Generic code is the exception: a `T?` whose `T` is an optional holds one ([generics](generics.md#optionals-inside-optionals)).

```calx
fn half(n: i32) -> i32? {
    if n % 2 == 0 {
        return n / 2      // error: write `some(...)`
    }
    return none
}

fn main() {
    let a = half(4)
    let b: i32 = a        // error: unwrap it with `??`, `!`, `if let` or `let ... else`
    let d = none          // error: can't tell what kind of optional `none` is
    println("{b} {d}")
}
```

## Reading one

| Written | Gives |
| --- | --- |
| `if let x = opt { } else { }` | Runs the first block with `x` bound to the value, or the `else` ([`if let`](patterns.md#if-let)) |
| `let x = opt else <leave>` | Binds `x`, or runs the `else`, which must leave: `return`, `break`, `continue` or a panic ([`let ... else`](patterns.md#let--else)) |
| `opt ?? b` | The value, or `b` if there's none. `b` runs only when it's needed |
| `opt!` | The value, or a panic: `unwrapped \`none\` with \`!\`` |
| `opt?.field` | `some` of the field, or `none` |
| `match opt { some(x) => ..., none => ... }` | Either, with patterns inside `some` ([patterns](patterns.md)) |

- In `a ?? b`, `b` is a `T` or a `T?`. With a `T` the result is a `T`. With a `T?` it's a `T?`, so `a ?? b ?? 0` tries `a`, then `b`, then gives 0.
- `??` next to any other binary operator needs parentheses: `(a ?? 0) + 1` ([strictness](strictness.md#parentheses)).
- `?.` on a field that's already an optional gives that optional, not one wrapped twice.
- Reading a field of an optional without `?.` or unwrapping is an error.

```calx
struct Player {
    name: str
    target: i32? = none
}

fn calls(n: i32) -> i32 {
    println("calls {n}")
    return n
}

fn main() {
    let p: Player? = some(Player { name: "Steve", target: some(3) })
    let gone: Player? = none
    println("{p?.target ?? 0} {gone?.target ?? 0} {p!.name}")   // out: 3 0 Steve
    let a = some(2) ?? calls(1)
    let b = gone?.target ?? calls(5)                            // out: calls 5
    println("{a} {b}")                                          // out: 2 5
}
```

## Comparing

`==` and `!=` compare two optionals: they're equal when both are `none`, or both hold equal values. An optional compares only with another optional, so it's `opt == some(3)`, not `opt == 3`. A float inside compares as floats do, so `some(NaN)` doesn't equal itself. `<` and the other orderings don't apply.

```calx
fn main() {
    let n: i32? = none
    let three: i32? = some(3)
    println("{n == none} {three == some(3)} {three != n}")   // out: true true true
}
```

## Where optionals come from

Besides functions that return them: [`T.try_from(x)`](conversions.md#try_from), the [`checked_` methods](integers.md#checked-and-saturating-arithmetic), and fields declared `T?`, which can default to `none`.

## In memory

A `T?` is a `T` followed by a `bool` saying whether it's there, so it costs no allocation and no indirection. `opt.map(f)` and `opt.or(x)` come with closures and generics.
