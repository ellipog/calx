---
title: Syntax
description: How Calx source is written - lines, comments, names, keywords and literals.
---

# Syntax

```calx
/// Doc comments start with three slashes.
fn main() {
    let greeting = "hello"   // a comment runs to the end of the line
    let total = 1 +
        2 +
        3
    println("{greeting}, {total}")   // out: hello, 6
}
```

## Files

A source file is UTF-8, and its name ends in `.calx`. It may start with `module a.b.c`, whose last part must be the file's name: `module example.basics` belongs in `basics.calx`. Without a `module` line, the module is named after the file.

## Lines and statements

- A statement ends at the end of its line. A `;` also ends one, so two short statements can share a line.
- A line that ends in an operator, a `,` or an opening bracket continues on the next line.
- Inside `(` `)` and `[` `]`, line breaks don't end anything, so argument lists can be split freely.
- `else` may start the line after an `if` block's `}`.

## Comments

- `//` starts a comment that runs to the end of the line.
- `///` starts a doc comment, which documents the declaration after it. Four or more slashes make an ordinary comment.

## Names

A name starts with a letter or `_`, and continues with letters, digits and `_`. Only ASCII letters are allowed for now.

A name that starts with `_` marks something unused on purpose ([variables](variables.md#unused-names)). `_` alone discards a value: `_ = f()`.

These keywords are reserved, including the ones whose features aren't built yet:

```text
actor and break checkout const continue dyn else enum extern fail false fn for if impl import
in interface let loop match module mut none not or own parallel pub return self send struct
true try type unsafe var while
```

The primitive type names (`i32`, `f64`, `bool` and the rest), and `print`, `println`, `panic`, `assert`, `some`, `ok` and `err`, are built in. A declaration or variable can't take one of these names.

## Integer literals

```calx
fn main() {
    println("{255} {0xFF} {0o377} {0b1111_1111}")    // out: 255 255 255 255
    println("{1_000_000} {7i64} {200u8}")             // out: 1000000 7 200
}
```

- Decimal, or hex, octal and binary with `0x`, `0o` and `0b`.
- `_` can go between digits.
- A suffix names the type: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64` or `usize`. Without one, the literal takes its type from where it's used, and is an `i32` when nothing decides ([integers](integers.md#literals)).

## Float literals

```calx
fn main() {
    println("{1.5} {2.0e3} {1.0e-3} {0.25f32}")    // out: 1.5 2000.0 0.001 0.25
}
```

- A float literal has a fraction, an exponent, or both: `1.5`, `2.0e3`, `1e-3`. A digit must follow the `.`, so `0..3` is a range.
- The suffix `f32` or `f64` names the type. Without one, the literal takes its type from where it's used, and is an `f64` when nothing decides ([floats](floats.md#literals)).

## Character and string literals

```calx
fn main() {
    let c: char = 'A'
    let r = '€'
    let n = 3
    println("{c} {r} tab:\t| brace: \{ quote: \" unicode: \u{1F600}")    // out: A € tab:	| brace: { quote: " unicode: 😀
    println("{n} squared is {n * n}")    // out: 3 squared is 9
}
```

- A character literal holds one character in single quotes. It's a `char` (one UTF-16 code unit, Java's `char`) where a `char` is expected, and otherwise a `rune` (a Unicode scalar value).
- A string literal is in double quotes, on one line. It's a `str`.
- `{expr}` inside a string inserts the value of `expr`. For now this works in the strings given to `print`, `println`, `panic` and `assert` ([printing](printing.md)).
- Escapes, in both: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`, `\'`, `\{`, `\}`, and `\u{...}` with 1 to 6 hex digits naming a Unicode scalar value.

`true` and `false` are the `bool` literals.

## Operators

From loosest to tightest:

| Operators | Notes |
| --- | --- |
| `..` `..=` | Ranges, in `for` loops and [patterns](patterns.md) |
| `or` | |
| `and` | Mixed with `or`, it needs parentheses ([strictness](strictness.md#parentheses)) |
| `not` | Prefix |
| `==` `!=` `<` `<=` `>` `>=` | Comparisons don't chain |
| `??` | An optional's value or a default ([optionals](optionals.md)). Next to another operator, it needs parentheses |
| `\|` | |
| `^` | |
| `&` | `&`, `\|` and `^` together need parentheses |
| `<<` `>>` `>>>` | Next to `+` or `-`, a shift needs parentheses |
| `+` `-` | |
| `*` `/` `%` | |
| `-` `~` | Prefix |
| `.field` `.method()` `f()` `a[i]` `!` `?.field` `.0` | Postfix. `!` and `?.` are for [optionals](optionals.md), `.0` for [tuples](tuples.md) |

`a op= b` is `a = a op b` for every arithmetic, bitwise and shift operator.

## Generic types in expressions

In an expression, `<` right after a name, with no space, starts the name's [generic types](generics.md): `choose<i64>(1, 2)`, `Pair<i32, str>.of(1, "a")`. With a space before it, `<` is less-than, so `a < b` is always a comparison. In a type, such as `let p: Pair<i32, str>`, spaces don't matter.
