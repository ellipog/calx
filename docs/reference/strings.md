---
title: Strings
description: str and String, literals that cost nothing, building strings, their methods, slicing by byte offset, Java's root-locale case mapping, going over runes and UTF-16 units, Java's hashCode and length, equality, patterns, and their C.
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
- A raw string, `#"..."#`, has no escapes, keeps its braces and backslashes as text, and inserts values with `#{...}`. One that starts at the end of a line is a block that can span lines and loses its closing line's indentation ([raw strings](syntax.md#raw-strings)). It's a `str` or `String` like any other literal.

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
| `s.find(t)`, `s.find_last(t)` | The byte offset where `t` first or last starts in `s`, a `usize?`. `""` is found at 0, or last at the end |
| `s.starts_with(t)`, `s.ends_with(t)` | Whether `s` begins or ends with `t` |
| `s.to_lower_root()`, `s.to_upper_root()` | A new `String`, as Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)` give ([case](#changing-the-case)) |
| `s.java_hash()` | Java's `String.hashCode`, an `i32`, for code that must match vanilla's hashes ([Java's view](#javas-view-of-a-string)) |
| `s.utf16_len()` | Java's `String.length()`: the UTF-16 units, a `usize` |
| `s.to_bytes()` | A copy of the bytes, a `List<u8>`, which `String.from_utf8` turns back into text |

- Methods that take text, `t` above, take a `str` or a `String`, since they only look at it.
- A `str` has the methods that only look: `len`, `is_empty`, `byte_at`, `contains`, `find`, `find_last`, `starts_with`, `ends_with`, `to_lower_root`, `to_upper_root`, `java_hash` and `utf16_len`.
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

## Slicing

`s[a..b]` is the text from byte `a` up to byte `b`, as `len()` and `byte_at` count. Every form of range works (D117):

| Written | Gives |
| --- | --- |
| `s[a..b]` | Bytes `a` up to `b` |
| `s[a..=b]` | Bytes `a` up to and including `b` |
| `s[a..]` | From byte `a` to the end |
| `s[..b]` | From the start up to byte `b` |
| `s[..]` | All of it |

```calx
fn main() {
    let id = "minecraft:stone"
    let colon = id.find(":") ?? 0
    println("{id[..colon]} {id[colon + 1..]}")    // out: minecraft stone
    let line = String("/give @p diamond 64")
    let space = line.find(" ") ?? line.len()
    let command = line[1..space]
    println("{command} {line[space + 1..].find(" ") ?? 0} {line[line.len() - 2..] == "64"}")    // out: give 2 true
}
```

- A part of a `str` is a `str`, the same bytes. A part of a `String` is a `String`.
- A `String`'s part is copied only where it's kept: in a variable, a field, a list, or a `return`. Where it's only looked at, it's a view of the same bytes, with nothing copied: printed, compared, passed as text to a method, looked up in a map, parsed, or sliced again.
- It panics unless `a <= b`, `b` is at most the length, and both are where runes start, so a part is always UTF-8. With a literal and constant offsets, the checker says so instead.
- Offsets are `usize`. `find` gives offsets that are always where runes start, since a match in UTF-8 can't start inside a rune.
- A part can't be assigned to or changed, as in `s[0..2] = "x"` or `s[1..].push('x')`. Build a new `String` from the parts.
- Only text can be sliced for now.

## Changing the case

`s.to_lower_root()` and `s.to_upper_root()` give the text in lowercase and uppercase exactly as Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)` do, which is what vanilla uses for names, identifiers and commands (D117):

```calx
fn main() {
    println("{"Stone_Bricks".to_lower_root()} {"straße".to_upper_root()}")    // out: stone_bricks STRASSE
    println("{"\u{130}".to_lower_root().len()} {"ΟΔΟΣ ΣΑΣ".to_lower_root()}")    // out: 3 οδος σας
}
```

- Each code point changes by itself, by Unicode 16's tables, as Java 25 has them, except in three ways Java has too:
  - A code point can become several. `ß` uppercases to `SS`, and `ﬃ` to `FFI`, which is why the length can change.
  - `İ`, U+0130, lowercases to an `i` and a combining dot above.
  - A capital sigma, `Σ`, lowercases to the final form `ς` when the word it's in has a cased letter before it and none after it, and to `σ` otherwise. Java finds that word with its word `BreakIterator`, so Calx splits the text into words by the same rules.
- Text that doesn't change comes back without a copy when it's a literal's: a `str`, or a `String` that owns no memory.
- ASCII goes eight bytes at a time. Each other code point is a lookup in a table of 30 KB, linked only into programs that change case.
- The tables come from Java itself: [`tools/unicode/CaseDump.java`](../../tools/unicode/CaseDump.java) asks Java 25 about every code point, and [`gen_case.py`](../../tools/unicode/gen_case.py) writes them into [runtime/calx_case.c](../../runtime/calx_case.c). The result matched Java on 2.4 million random strings, weighted to sigmas, cased letters and every kind of code point the word rules treat differently.
- Calx doesn't have the other locales' rules, such as Turkish's dotless `ı`, since vanilla only uses the root locale.

## Going over the text

`for r in s.runes()` goes over the runes, the Unicode scalar values. `for c in s.chars()` goes over the UTF-16 units, as Java's `charAt` does, so a rune past U+FFFF is two `char`s, a high surrogate and then a low one:

```calx
fn main() {
    let s = "aé\u{1F600}"
    var runes = 0
    for r in s.runes() {
        runes += 1
        print("{u32(r)} ")
    }
    println("({runes} runes)")    // out: 97 233 128512 (3 runes)
    for c in s.chars() {
        print("{u16(c)} ")
    }
    println("({s.utf16_len()} units)")    // out: 97 233 55357 56832 (4 units)
}
```

- The loop decodes the text as it goes, with nothing allocated, since text is always UTF-8.
- `runes()` and `chars()` are only what a `for` loop goes over, and a string itself isn't, so the loop says which it means.
- The string can't change while the loop goes over it, as with a list.

## Java's view of a string

Java's strings are UTF-16, and vanilla hashes and measures them that way. `s.java_hash()` and `s.utf16_len()` give what Java's `hashCode()` and `length()` give for the same text, without converting it:

```calx
fn main() {
    println("{"hello".java_hash()} {"minecraft:stone".java_hash()}")    // out: 99162322 -1133948840
    println("{"\u{1F600}".len()} {"\u{1F600}".utf16_len()}")    // out: 4 2
}
```

- `java_hash` is what a [`JavaHashMap`](java-maps.md) and an [`OpenHashMap`](fastutil-maps.md) place a string key by, and what vanilla code that hashes names computes. Every type with Java's `hashCode` has it ([`JavaHash`](java-maps.md#javahash)).
- `utf16_len` is what the protocol's string limits count, such as 16 for a player name and 32767 for a chat message.
- They matched Java 25 on 20,000 random strings, with U+0000, every length of UTF-8, and surrogate pairs.
- `java_hash` takes eight ASCII bytes a step, `h * 31^8` plus each byte times its power, so the chain of multiplies is an eighth as long as Java's loop.
- [Modified UTF-8](../std/text-mutf8.md), which NBT uses, is in `std.text.mutf8`.

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

## In C

- A `str` is `cx_str`, a pointer and a length.
- A `String` is `cx_String`, a pointer, a length and a capacity. A capacity of 0 means it doesn't own its bytes: it's a literal, or empty. Freeing it frees the bytes only when it owns them.
- A string grows by doubling, and starts at 16 bytes.
- Building `"n = {n}"` sends the printing functions' output into the new string, so building a string and printing one share the same code, and floats appear exactly as Java prints them ([floats](floats.md#printing)).
- A part, `s[a..b]`, is `cx_str_slice(s, a, b, inclusive, at)`, which checks the offsets and gives a `cx_str` of the same bytes. Where the part is kept as a `String`, `cx_string_from` copies it. A `String` that's a variable is viewed after the offsets are worked out, through a pointer, so the offsets run first even when they change it:

```c
// let command = line[1..space], from the example above
cx_String command = cx_string_from(({ const cx_String* cx_t19 = &line; cx_usize cx_t17 = ((cx_usize)1ULL);
    cx_usize cx_t18 = space; cx_str cx_t16 = cx_sv(*cx_t19); cx_str_slice(cx_t16, cx_t17, cx_t18, 0, "main.calx:7:19"); }));
```

- `find` takes each place the first byte of what it looks for is, with `memchr`, which goes many bytes a step, and compares the rest only there. `contains` is `find`.
