---
title: Strictness
description: The rules that make Calx's checker stricter than Rust's, and why.
---

# Strictness (D63)

Calx's checker is stricter than Rust's. Calcite ports Java, and silent conversions and precedence slips are exactly where bit-exactness breaks, so the compiler rejects them. Every rule is an error, and no flag turns one off. There are no warnings, except for deprecated APIs once there are any.

Each rule is on the page of the feature it's about:

| Rule | Page |
| --- | --- |
| `T(x)` only converts exactly. Lossy conversions are named: `wrap`, `saturate`, `nearest` | [Conversions](conversions.md) |
| A conversion is exact only if it is at both of `usize`'s widths | [Conversions](conversions.md#usize) |
| Indexes are `usize` | [Types](types.md#arrays) |
| A literal must fit its type, hex included. Unsigned types take no negative literal | [Integers](integers.md#literals) |
| A constant expression that overflows is an error | [Integers](integers.md#constant-expressions) |
| A constant shift count must be under the type's width | [Integers](integers.md#operators) |
| Comparing with a NaN constant is an error | [Floats](floats.md#constants) |
| Unused variables, parameters, functions, structs and constants are errors, and so are a `var` that never changes and a `mut` parameter that's never changed | [Variables](variables.md#unused-names) |
| No shadowing | [Variables](variables.md#no-shadowing) |
| Code that can never run is an error | [Control flow](control-flow.md#unreachable-code) |
| A function returns with `return`, on every path | [Functions](functions.md#returning) |
| A `T` never becomes a `T?` by itself: write `some(x)`. An optional compares only with another optional | [Optionals](optionals.md#making-one) |
| `??` next to another operator needs parentheses | [Optionals](optionals.md#reading-one) |
| A `match` covers every value, and every arm can match | [Patterns](patterns.md#every-case-every-arm-d63) |
| A capitalized name in a pattern is a variant or constant, never a binding. A record pattern names every field or ends in `..` | [Patterns](patterns.md#names-in-patterns) |
| `let` without `else` needs a pattern that always matches, and `let ... else` and `if let` one that might not | [Patterns](patterns.md#let--else) |
| A value can't be used after it moves, or after it might have moved, and a move in a loop needs a new value before the next pass | [Ownership](ownership.md#moves) |
| Only what a function owns can move: views, loop elements and pattern names that only look can't. Neither can list elements or fields of views | [Ownership](ownership.md#views) |
| A value can't change or move while a pattern's names are views into it | [Ownership](ownership.md#patterns) |
| `.copy()` is only for values that own memory, and `[x; n]` copies only a `Copy` `x` | [Ownership](ownership.md#moves) |
| Generic code is checked where it's written and uses only what its bounds give. A call whose types can't be worked out is an error, never a guess. Unused generic parameters are errors | [Generics](generics.md#errors) |
| An impl matches its interface exactly, parameter names included, and has nothing else. Unused impls are errors. A name two interfaces share needs `Iface.method(x)` | [Interfaces](interfaces.md#implementing-one) |
| A type has one function of each name: its own impl can't reuse a name its interfaces give it | [Interfaces](interfaces.md#implementing-one) |
| Unused imports are errors, and an import can't shadow a name | [Modules](modules.md#importing) |
| Private fields and methods stay in their module, and a `pub` signature or field shows only `pub` types | [Modules](modules.md#pub) |
| An impl goes in its type's module or its interface's | [Modules](modules.md#where-impls-go) |

## Nothing is dropped silently

An expression on its own line must have the unit type `()`. A value nobody wants is discarded explicitly with `_ = f()`, so an ignored result is always visible.

```calx
fn three() -> i32 {
    return 3
}

fn main() {
    three()        // error: this `i32` value is unused; discard it with `_ = ...`
    _ = three()
}
```

## Parentheses

These mixtures need parentheses, since their precedence surprises people:

- `and` with `or`: `a or (b and c)`
- `&`, `|` and `^` with each other: `(a & b) | c`
- a shift with `+` or `-`: `1 << (n - 1)`, the classic slip
- `??` with any other operator: `(a ?? 0) + 1`

Comparisons don't chain: `a < b < c` doesn't compile.

```calx
fn main() {
    let a = true
    let n = 3
    let _x = a or a and a    // error: `and` and `or` together need parentheses
    let _y = 1 << n - 1      // error: a shift next to `+` or `-` needs parentheses
}
```

## Conditions

A literal `true` or `false` as a condition is an error, since one branch could never run, and `while true` is written `loop`.

The rest of D63 applies to features that aren't built yet, and is in the [spec](../CALX_SPEC.md#strictness-d63).
