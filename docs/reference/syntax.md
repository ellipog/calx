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

A source file is UTF-8, and its name ends in `.calx`. A first line that starts with `#!`, such as `#!/usr/bin/env calx`, is skipped, so the file can run as a script by its own name ([running a file](command.md#running-a-file)). Each file is a [module](modules.md). It can start with `module a.b.c`, which must match its place: `module game.world` is `game/world.calx` under the source root. A file at the root can leave the line out, and is then named after the file.

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
and break checkout const continue dyn else enum extern fail false fn for if impl import in
interface let loop match module mut none not or own parallel pub return self struct true try
type unsafe var while
```

The primitive type names (`i32`, `f64`, `bool` and the rest), and `print`, `println`, `eprint`, `eprintln`, `panic`, `assert`, `some`, `ok` and `err`, are built in. A declaration or variable can't take one of these names.

Any other name works, including ones that mean something to the C that Calx compiles to: C's keywords, such as `int` and `register`, and the names C compilers define for themselves on some systems, such as `unix` and `linux`. Calx renames them in the C it writes, so a program builds the same on every system:

```calx
struct Flags {
    unix: bool
    register: i32
}

fn main() {
    let linux = 2
    let f = Flags { unix: true, register: linux }
    println("{f.unix} {f.register}")    // out: true 2
}
```

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
- `{expr}` inside a string inserts the value of `expr`, which makes the literal a `String` ([strings](strings.md#literals)). An empty `{}` is an error. `{expr:spec}` writes it with a format, such as `{x:.2}` or `{n:>8}` ([formats](printing.md#formats)); the first `:` outside brackets starts the format.
- Escapes, in both: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`, `\'`, `\{`, `\}`, and `\u{...}` with 1 to 6 hex digits naming a Unicode scalar value.

`true` and `false` are the `bool` literals.

### Byte literals

`b'0'` is a byte literal: the `u8` that an ASCII character is, here 48. It holds one ASCII character, one of the escapes above below 128, or `\xHH` with two hex digits for any byte. It's an integer literal, the same as writing `48u8`, so it can be a constant, a pattern or an array element, for file formats whose fields are ASCII.

```calx
const DIR: u8 = b'5'

fn main() {
    let header = [b'P', b'K', b'\x03', b'\x04']
    let flag = b'5'
    let kind = match flag {
        b'0' | b'\0' => "file",
        DIR => "directory",
        _ => "other",
    }
    println("{header} {kind} {b'a' - b'A'} {b'\n'} {b'\xFF'}")    // out: [80, 75, 3, 4] directory 32 10 255
}
```

### Raw strings

A raw string is written `#"..."#`. It has no escapes, and its braces and backslashes are just text, so it suits code, JSON and paths. `#{expr}` inserts a value (D125).

```calx
fn main() {
    let name = "Point"
    println(#"C:\temp\{name}.json"#)                  // out: C:\temp\{name}.json
    println(#"impl #{name} { fn show(self) {} }"#)    // out: impl Point { fn show(self) {} }
    println(#"say "hi" to #{name.len()} people"#)    // out: say "hi" to 5 people
    println(##"a raw string ends at "#, so this one has ##"##)    // out: a raw string ends at "#, so this one has ##
}
```

- `#"` starts one and `"#` ends it, except a `"#` followed by `{`, which is a quote and then an insertion: `#"f("#{x}")"#` is `f("`, `x`, and `")`.
- More `#`s hold `"#`. Between `##"` and `"##` the insertion is `##{expr}`, and `#{` is text. Any number of `#`s works.
- A raw string that isn't a block stays on one line, like an ordinary string.
- `{}` inside one is text. An empty `#{}` is an error.

A raw string whose `#"` ends its line is a block. Its text is the lines that follow, up to a line holding only the closing `"#`, and that line's indentation comes off every line. So the text can be indented with the code around it:

```calx
fn main() {
    let name = "Point"
    let code = #"
        impl #{name} {
            fn show(self) {}
        }
        "#
    print(code)
    println("|")
    // out: impl Point {
    // out:     fn show(self) {}
    // out: }|
}
```

- The newline before the closing line isn't part of the text, so `code` above ends with `}`.
- Every line with text must start with the closing line's indentation, character for character, so a tab doesn't stand in for spaces. One that doesn't is an error, as is text before the closing `"#` on its line. A blank line may be indented less.
- The text is the same on every system: a `\r\n` in the file becomes `\n`.

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

On a struct or an enum, `+`, `-`, `*`, `/`, `%` and a prefix `-` call its impl of [`Add`, `Sub`, `Mul`, `Div`, `Rem` or `Neg`](interfaces.md#operators).

The logical operators are words. `&&`, `||` and a prefix `!` are errors that say to write `and`, `or` and `not`, since `!` after a value unwraps an optional.

## Generic types in expressions

In an expression, `<` right after a name, with no space, starts the name's [generic types](generics.md): `choose<i64>(1, 2)`, `Pair<i32, str>.of(1, "a")`. With a space before it, `<` is less-than, so `a < b` is always a comparison. In a type, such as `let p: Pair<i32, str>`, spaces don't matter.
