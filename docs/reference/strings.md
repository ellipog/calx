---
title: Strings
description: str and String, literals that cost nothing, building strings, their methods, equality, patterns, and their C.
---

# Strings

Calx has two string types. `str` is a read-only view of UTF-8 text, and `String` owns its text and can change it.

```calx
fn main() {
    let name: str = "Steve"
    var s = String("hi ")
    s.push_str(name)
    s.push('!')
    let line = "{s} ({s.len()} bytes)"    // a `String` built from parts
    println(line)                         // out: hi Steve! (9 bytes)
    println("{line.starts_with("hi")} {line == "hi"}")    // out: true false
}
```

## Literals

- A string literal is a `str`, or a `String` where one is wanted: `let s: String = "x"`, a `String` parameter or field, or `xs.push("x")` on a `List<String>`.
- A literal used as a `String` costs nothing. The `String` points at the literal's bytes and owns no memory until it changes, when it copies them into memory of its own.
- A literal with `{...}` parts is a `String`, built when it runs. As the message of `print`, `println`, `assert` and `panic`, its parts are printed directly instead, with no string built ([printing](printing.md)).
- Escapes and the rest of the literal syntax are in [syntax](syntax.md#character-and-string-literals).

## Making one

| Written | Gives |
| --- | --- |
| `String()` | An empty string, which owns no memory yet |
| `String(s)` | A copy of the `str` `s`. Free when `s` is a literal |
| `"x"` where a `String` is wanted | The literal, for free |
| `"n = {n}"` | A new string, built from its parts |
| `s.copy()` | A copy that owns its own memory ([ownership](ownership.md)) |
| `String.from_utf8(bytes)` | The `List<u8>` as a `String`, or `none` if it isn't UTF-8. The string takes the list's memory, so nothing is copied |

`String.from_utf8` accepts exactly the UTF-8 that encodes Unicode scalar values: no overlong forms, no surrogates, and nothing past U+10FFFF. Java's `new String(bytes, UTF_8)` puts U+FFFD in place of bad bytes instead; a lossy version is designed with the rest of `std.text`.

```calx
fn main() {
    let euro: List<u8> = [0xE2, 0x82, 0xAC]
    let bad: List<u8> = [0xC0, 0x80]
    println("{String.from_utf8(euro) ?? "?"} {String.from_utf8(bad) ?? "?"}")    // out: € ?
    let word: str = "hey"
    println("{word.byte_at(0)} {word.len()}")    // out: 104 3
}
```

## Methods

| Method | Does |
| --- | --- |
| `s.len()` | The length in bytes, a `usize` |
| `s.is_empty()` | Whether the length is 0 |
| `s.byte_at(i)` | The byte at offset `i`, a `u8`. Panics if `i` is past the end |
| `s.push(r)` | Adds the `rune` `r`, as UTF-8 |
| `s.push_str(t)` | Adds the text `t` |
| `s.clear()` | Removes all the text, keeping the memory |
| `s.reserve(n)` | Makes room for `n` more bytes, so the next pushes don't allocate |
| `s.contains(t)` | Whether `t` appears in `s`. Every string contains `""` |
| `s.starts_with(t)`, `s.ends_with(t)` | Whether `s` begins or ends with `t` |

- Methods that take text, `t` above, take a `str` or a `String`, since they only look at it.
- A `str` has the methods that only look: `len`, `is_empty`, `byte_at`, `contains`, `starts_with` and `ends_with`.
- Methods that change the string need a variable declared with `var`, or a `mut` parameter.
- `s.as_ptr()` points at the bytes, for [unsafe code](unsafe.md#pointers).
- `s.push_str(s)` is an error, since `s` would change while being read. Push a copy.

```calx
fn main() {
    var out = String()
    out.reserve(16)
    for i in 0..3 {
        out.push_str("{i},")
    }
    out.push('é')
    println("{out} {out.len()}")    // out: 0,1,2,é 8
    out.clear()
    println("[{out}] {out.is_empty()}")    // out: [] true
}
```

## Comparing and matching

`==` and `!=` compare the text, wherever it's stored: a literal, a built string and a copy are equal when they hold the same bytes. Both sides must have the same type, so a `String` compares with a `String`, including a literal used as one.

A `match` can test a `str` or a `String` against string literals. Since any text can come in, it needs a `_` arm:

```calx
fn command(s: String) -> i32 {
    return match s {
        "stop" => 0,
        "go" => 1,
        _ => -1,
    }
}

fn main() {
    let typed = String("go")
    println("{command(typed)} {command("stop")} {command("jump")}")    // out: 1 0 -1
}
```

## Printing

`{s}` in a string shows the text. `println(s)` takes a `str` or a `String`.

## Not built yet

Slicing by byte offset, iterating runes and UTF-16 units, Java's `hashCode` and `length()`, Modified UTF-8, case mapping, and `Display` for interpolating other types are designed ([spec](../CALX_SPEC.md#strings-d28)).

## In C

- A `str` is `cx_str`, a pointer and a length.
- A `String` is `cx_String`, a pointer, a length and a capacity. A capacity of 0 means it doesn't own its bytes: it's a literal, or empty. Freeing it frees the bytes only when it owns them.
- A string grows by doubling, and starts at 16 bytes.
- Building `"n = {n}"` sends the printing functions' output into the new string, so building a string and printing one share the same code, and floats appear exactly as Java prints them ([floats](floats.md#printing)).
