---
title: Strings
description: The str and String types, literals that cost nothing, building strings, their methods, slicing, Java's case mapping, runes and UTF-16 units, and their C.
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

## A `str` where a `String` is wanted

A `str` goes where a `String` is only looked at: a parameter taken as it is, not `own` or `mut`, such as the text `Regex.find` searches, and `contains` on a `List<String>`. It goes as a `String` that views the same bytes, so nothing is copied, and a `copy` of it copies them. Text also compares with text: `==`, `!=`, `<` and the rest take a `str` and a `String`, as views of their bytes.

```calx
fn shout(s: String) -> String {
    var out = s.copy()
    out.push('!')
    return out
}

fn main() {
    let word = "hey"    // a `str`
    let names: List<String> = ["ann", "hey"]
    let owned = String("hey")
    println("{shout(word)} {names.contains(word)} {owned == word}")    // out: hey! true true
}
```

Where the `String` is kept or changed, the `str` needs its own copy, `String(word)`: an `own` or `mut` parameter, a variable or field of type `String`, or `push` on a `List<String>`.

## Making one

| Written | Gives |
| --- | --- |
| `String()` | An empty string, which owns no memory yet |
| `String(s)` | A copy of the `str` `s`. Free when `s` is a literal |
| `"x"` where a `String` is wanted | The literal, for free |
| `"n = {n}"` | A new string, built from its parts |
| `s.copy()` | A copy that owns its own memory ([ownership](ownership.md)) |
| `String.from_utf8(bytes)` | The `List<u8>` as a `String`, or `none` if it isn't UTF-8. The string takes the list's memory, so nothing is copied |

`String.from_utf8` accepts exactly the UTF-8 that encodes Unicode scalar values: no overlong forms, no surrogates, and nothing past U+10FFFF. Java's `new String(bytes, UTF_8)` puts U+FFFD in place of bad bytes instead, which [`std.text.utf8.decode_lossy`](../std/text-utf8.md) does too.

The check, which `fs.read_text` makes too, goes eight ASCII bytes at a time, and after eight, 64 at a time while they last. A text that's all ASCII is checked about as fast as memory reads: 113 MB of access log in 5.6 ms, where eight at a time took 7.8 ms (CLX-309).

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
| `s.truncate(n)` | Keeps the first `n` bytes, and the memory. Does nothing if it has `n` or fewer. Panics if byte `n` is inside a rune |
| `s.reserve(n)` | Makes room for `n` more bytes, so the next pushes don't allocate |
| `s.contains(t)` | Whether `t` appears in `s`. Every string contains `""` |
| `s.find(t)`, `s.find_last(t)` | The byte offset where `t` first or last starts in `s`, a `usize?`. `""` is found at 0, or last at the end |
| `s.starts_with(t)`, `s.ends_with(t)` | Whether `s` begins or ends with `t` |
| `s.to_lower_root()`, `s.to_upper_root()` | A new `String`, as Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)` give ([case](#changing-the-case)) |
| `s.java_hash()` | Java's `String.hashCode`, an `i32`, for code that must match Java's hashes ([Java's view](#javas-view-of-a-string)) |
| `s.utf16_len()` | Java's `String.length()`: the UTF-16 units, a `usize` |
| `s.to_bytes()` | A copy of the bytes, a `List<u8>`, which `String.from_utf8` turns back into text |
| `s.interned()` | The text as a `String` that shares one copy of it, never freed, with every other interned string of the same text, as Python's `sys.intern` does ([interned strings](#interned-strings)) |
| `s.trim()`, `s.trim_start()`, `s.trim_end()` | The text without the whitespace at both ends, the start or the end, as a part of `s` ([taking text apart](#taking-text-apart)) |
| `s.trim_prefix(t)`, `s.trim_suffix(t)` | The text without `t` at the start or the end, once, if it's there; otherwise all of it. A part of `s` |
| `s.split(t)` | The parts between each `t`, a `List<String>`, empty parts too. `""` splits into the runes |
| `s.split_once(t)` | The parts before and after the first `t`, a `(String, String)?`, or `none` if there's no `t` |
| `s.split_whitespace()` | The words between whitespace, a `List<String>`, with no empty ones |
| `s.lines()` | The lines, a `List<String>`, each without the `\n`, `\r\n` or `\r` that ends it |
| `s.replace(from, to)` | A new `String` with every `from` replaced by `to`. `""` puts `to` between the runes and at both ends |
| `s.repeat(n)` | A new `String` of `s` `n` times |
| `s.pad_start(width, r)`, `s.pad_end(width, r)` | A new `String` of at least `width` runes, with the `rune` `r` added at the start or the end. In a string, `{s:>8}` and `{s:<8}` pad as they print ([formats](printing.md#formats)) |
| `xs.join(t)` | A list of `String`s or of `str`s as one `String`, with `t` between them |

- Methods that take text, `t` above, take a `str` or a `String`, since they only look at it.
- `truncate` takes back what pushes added: `let at = s.len()`, pushes, then `s.truncate(at)`.
- A `str` has the methods that only look: `len`, `is_empty`, `byte_at`, `contains`, `find`, `find_last`, `starts_with`, `ends_with`, `to_lower_root`, `to_upper_root`, `java_hash`, `utf16_len` and `interned`, and the ones that give new text or parts, from `trim` to `pad_end`. A `str`'s trimmed part is a `str`.
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
- A `String`'s part is copied only where it's kept: in a field, a list, or a `return`, or in a variable that's moved or whose text changes while it's in scope. Where it's only looked at, it's a view of the same bytes, with nothing copied: printed, compared, passed as text to a method or to a function that reads it, looked up in a map, parsed, sliced again, or kept with `let` in a name that's only looked at while the text stays as it is.
- It panics unless `a <= b`, `b` is at most the length, and both are where runes start, so a part is always UTF-8. With a literal and constant offsets, the checker says so instead.
- Offsets are `usize`. `find` gives offsets that are always where runes start, since a match in UTF-8 can't start inside a rune.
- A part can't be assigned to or changed, as in `s[0..2] = "x"` or `s[1..].push('x')`. Build a new `String` from the parts.
- A list or an array slices too, into a new list of copies of its elements ([slices](lists.md#slices)).

## Taking text apart

`trim`, `split`, `lines`, `replace` and `join` take text apart and put it back together, as a script reads a CSV, a config file or a log:

```calx
fn main() {
    let config = "name = calx\n  # a comment\nport=25565\r\nmotd = Hello, world \n"
    for line in config.lines() {
        let t = line.trim()
        if t.is_empty() or t.starts_with("#") {
            continue
        }
        if let (key, value) = t.split_once("=") {
            println("{key.trim()}: [{value.trim()}]")
        }
    }
    // out: name: [calx]
    // out: port: [25565]
    // out: motd: [Hello, world]
    let row = "a, b ,,c"
    let cells = row.split(",")
    println("{cells.len()} {row.split_whitespace().join("_")} {"x".repeat(3)} {"7".pad_start(3, '0')}")    // out: 4 a,_b_,,c xxx 007
    println("{"a-b-c".replace("-", " - ")} {"v2.1".trim_prefix("v")}")    // out: a - b - c 2.1
}
```

- Whitespace is what Unicode calls `White_Space`: ASCII's space, tab, line breaks, vertical tab and form feed, and the other spaces, such as U+0085, U+00A0, U+2028 and U+3000. Rust's `trim` takes the same. Python's `strip` takes the separators U+001C to U+001F too, and Java's `strip` takes those as well but keeps U+0085, U+00A0, U+2007 and U+202F.
- `trim` and its kin give a part of the text, as [slicing](#slicing) does: a view where it's only looked at, and a copy where it's kept.
- `split`, `split_whitespace` and `lines` give a new `List<String>`, and `split_once` a pair. In a `for` loop they make no list: the loop goes over the parts in place, as views of the text, with nothing copied.
- Kept with `let` or `if let`, as in `let fields = line.split(",")` or `let (key, value) = entry.split_once("=") ?? ...`, the parts are views of the text too, so only the list is new, when the compiler sees that the names are only looked at and the text doesn't change or move while they're in scope. Otherwise each part is a copy. The program does the same either way; a view only saves the copies.
- A `var` given a copy or a part of text, as in `var path = parts[1].copy()` and then `path = path[..q]`, holds a view of the text too, when the var is never changed in place, read by a closure or taken apart by a pattern, and the text is in a name declared before it that nothing moves or changes. A part of the var is a view while it holds one, and a copy once it's been given a string of its own. Where the var moves, into a map, a list or a `return`, a view's bytes are copied then, so they outlive the text.
- A list from `split` kept with `let` and only ever indexed by number, as in `fields[0]` and `fields[2]`, is split only as far as the highest index, so the ends of the parts after it aren't looked for. An index past the parts there are fails as it would on the whole list, with the whole list's length. Used any other way, by `len`, a loop, a function it's passed to or a closure, the list is whole.
- `lines` ends a line at `\n`, `\r\n` or a lone `\r`, as Java's `BufferedReader.readLine` and [`std.io`](../std/io.md)'s lines do, and a line break at the very end doesn't start another line. `split("\n")` gives an empty last part there instead.
- `xs.split(t).join(u)`, `s.lines().join(u)` and `s.split_whitespace().join(u)` put the parts straight into the new text, with no list made.
- `pad_start` and `pad_end` count runes, not bytes, so `"é".pad_start(3, '*')` is `**é`.

```calx
fn main() {
    var text = String("alpha, beta\ngamma")
    var kept = List<String>()
    for line in text.lines() {
        for cell in line.split(",") {
            kept.push(cell.trim())    // the part is copied here, where it's kept
        }
    }
    text.clear()
    println(kept.join("|"))    // out: alpha|beta|gamma
}
```

- In such a loop, each part is a view of the text, so the text can't change while the loop goes over it, as with a list, and a part can't be moved out of the loop: `kept.push(cell)` is an error. Keep a copy, `cell.copy()`, or anything made from it, such as `cell.trim()` or `"{cell}"`.

## Changing the case

`s.to_lower_root()` and `s.to_upper_root()` give the text in lowercase and uppercase exactly as Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)` do, which is what Java code uses for identifiers and commands (D117):

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
- The tables come from Java itself: [`tools/unicode/CaseDump.java`](../../tools/unicode/CaseDump.java) asks Java 25 about every code point, and [`gen_case.calx`](../../tools/unicode/gen_case.calx) writes them into [runtime/calx_case.c](../../runtime/calx_case.c). The result matched Java on 2.4 million random strings, weighted to sigmas, cased letters and every kind of code point the word rules treat differently.
- Calx doesn't have the other locales' rules, such as Turkish's dotless `ı`: programs that need the same text everywhere use the root locale.

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
- `for b in s.to_bytes()` goes over the bytes, each a `u8`, where they are: the loop makes no list and copies nothing, where `to_bytes()` anywhere else is a copy.
- The string can't change while the loop goes over it, as with a list.

## Java's view of a string

Java's strings are UTF-16, and Java code hashes and measures them that way. `s.java_hash()` and `s.utf16_len()` give what Java's `hashCode()` and `length()` give for the same text, without converting it:

```calx
fn main() {
    println("{"hello".java_hash()} {"minecraft:stone".java_hash()}")    // out: 99162322 -1133948840
    println("{"\u{1F600}".len()} {"\u{1F600}".utf16_len()}")    // out: 4 2
}
```

- `java_hash` is what a [`JavaHashMap`](java-maps.md) and an [`OpenHashMap`](fastutil-maps.md) place a string key by, and what Java code that hashes names computes. Every type with Java's `hashCode` has it ([`JavaHash`](java-maps.md#javahash)).
- `utf16_len` is what the protocol's string limits count, such as 16 for a player name and 32767 for a chat message.
- They matched Java 25 on 20,000 random strings, with U+0000, every length of UTF-8, and surrogate pairs.
- `java_hash` takes eight ASCII bytes a step, `h * 31^8` plus each byte times its power, so the chain of multiplies is an eighth as long as Java's loop.
- [Modified UTF-8](../std/text-mutf8.md), which NBT uses, is in `std.text.mutf8`, and bytes that may not be UTF-8 are read as Java reads them by [`std.text.utf8`](../std/text-utf8.md).

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

## Interned strings

`s.interned()` gives the text as a `String` that shares one copy of it with every other interned string of the same text, as Python's `sys.intern` and Java's `intern` do. The copy is never freed, and the string owns no memory, as a literal's doesn't. Copying it costs nothing, and changing it copies the text first, so the others keep theirs. A program that holds the same few names many times, such as the tags of a parsed document, holds their bytes once, and comparing them reads bytes that are already in the cache.

```calx
fn main() {
    let line = String("tag=book")
    var tags: List<String> = []
    for _ in 0..3 {
        tags.push(line[4..].interned())
    }
    var first = tags[0].copy()
    first.push_str("s")
    println("{first} {tags[0]} {tags[2]}")    // out: books book book
}
```

- Only names are kept, since they're never freed: text of 1 to 64 bytes, up to 65,536 of them and 1 MiB in all. Empty text, longer text and text past those limits comes back as an ordinary copy, so `interned` is always safe to call, and saves memory only for text that repeats.
- [`std.xml`](../std/xml.md) interns tags and attribute names, [`std.json`](../std/json.md) object keys, and [`std.nbt`](../std/nbt.md) compound keys.
- Any thread can call it. Each thread keeps the last names it was given, so a name it has met before costs a hash and a compare, and takes no lock.

## Printing

`{s}` in a string shows the text. `println(s)` takes a `str` or a `String`.

## In C

- A `str` is `cx_str`, a pointer and a length.
- A `String` is `cx_String`, a pointer, a length and a capacity. A capacity of 0 means it doesn't own its bytes: it's a literal, or empty. Freeing it frees the bytes only when it owns them.
- A string grows by doubling, and starts at 16 bytes.
- `push` and `push_str` compile to inline C, a check for room and then a store or a copy. Growing is a cold call to the runtime, `cx_string_grow_to`, given a copy of the capacity, as a list's is, so a string in a local keeps its pointer, length and capacity in registers ([lists](lists.md#in-c), D158). A literal's string has a capacity of 0, so its first push always takes the call, which copies the literal's bytes into a buffer of its own.
- A rune that isn't ASCII is written with no branch on its length, when there are 4 bytes of room: all 4 are written, and the length counts the ones that belong to it. Text that isn't ASCII mixes lengths, so a branch on each would often guess wrong. A rune with less room than that goes through a call (D160).
- A function with a loop that's given a `String` to change works on a copy of it, as one given a list does ([lists](lists.md#in-c), D159).

A million values pushed onto a string, in ms, best of 50, from [`bench/strings`](../../bench/strings), on an i9-13900K pinned to one P-core:

| | Calx | Calx, branching on length | Rust |
| --- | --- | --- | --- |
| An ASCII rune each, onto a new string in a local | 0.54 | 1.69 | 0.67 |
| A rune each of 1, 2, 3 or 4 bytes | 2.86 | 7.16 | 6.22 |
| A word each, of 3 to 7 bytes | 6.51 | 7.28 | 8.23 |
| An ASCII rune each, onto a string given to change, not inlined | 0.48 | 1.64 | 0.57 |
| A text escaped as JSON, a rune at a time, onto a string given, not inlined | 1.94 | 2.94 | 2.06 |

The middle column is Calx as it was before it wrote runes without a branch and copied a `String` given to change (D160). Rust's `push` branches on a rune's length, so it guesses wrong on the mixed runes as Calx did before. A word is copied by `memcpy`, a call when the length isn't known until it runs, in both.
- Building `"n = {n}"` sends the printing functions' output into the new string, so building a string and printing one share the same code, and floats appear exactly as Java prints them ([floats](floats.md#printing)).
- A part, `s[a..b]`, is `cx_str_slice(s, a, b, inclusive, at)`, which checks the offsets and gives a `cx_str` of the same bytes. Where the part is kept as a `String`, `cx_string_from` copies it. A `String` that's a variable is viewed after the offsets are worked out, through a pointer, so the offsets run first even when they change it:

```c
// let command = line[1..space], from the example above
cx_String command = cx_string_from(({ const cx_String* cx_t19 = &line; cx_usize cx_t17 = ((cx_usize)1ULL);
    cx_usize cx_t18 = space; cx_str cx_t16 = cx_sv(*cx_t19); cx_str_slice(cx_t16, cx_t17, cx_t18, 0, "main.calx:7:19"); }));
```

- `find` picks the two bytes of what it looks for that are likely rarest in text, by a table of how common each byte is, and tests 32 places at a time with AVX2, or 16 with SSE2 or NEON, for both at once, comparing the rest only where both are. So `example`, whose first byte is common, is found about as fast as text whose every byte is rare. A one-byte needle is found by `memchr`, and without SIMD, `memchr` finds the rarest byte. `contains` is `find`.
- `find` and `split_once` with a one-byte needle, a space, a quote or a colon, are `cx_text_find`, inline C that looks over the first 64 bytes sixteen at a time, as a `for` loop's `split` does, and hands the rest to `find`'s `memchr`, which is quicker on long runs. The end of a field is mostly that close, and four calls deep to `memchr` cost a line of an access log more than its search did (CLX-309).
- `trim` and its kin are parts, as `s[a..b]` is: `cx_text_trim` gives a `cx_str` of the same bytes, copied by `cx_string_from` only where it's kept. `trim_prefix` and `trim_suffix` are inline C.
- A `for` loop over `split`, `lines` or `split_whitespace` is a `while` over inline C that gives the next part, a `cx_str`, and where the one after starts. A loop variable is a `cx_String` with the part's pointer and length and a capacity of `CX_VIEW_CAP`, the top bit, which marks a view: freeing it frees nothing, and a copy of it copies the bytes, where a literal's capacity of 0 would share them.
- A `str` passed for a `String` is `cx_str_view`, the same kind of `cx_String`, built in a temporary that the function gets a pointer to, so the call copies no bytes.
- A part kept with `let` is a view, `cx_str_view` of the part's `cx_str`, when the ownership pass (moves.cpp) finds two things (CLX-293). First, no name the `let` binds is moved anywhere in the function, and none is a `var` or `mut`. Second, the local the text is in, through parts of parts, isn't moved, changed or assigned to while the names are in scope. A change through a `mut` view of one of its parts counts. Text that's a literal's or a constant's always lasts. `split`, `lines` and `split_whitespace` take a flag that makes each part of the list a view. A part passed to a function that reads it is a view the same way, unless the text's local is moved or changed anywhere in the same statement or condition, whose end drops the temporary. Since a view frees nothing and a copy of one copies its bytes, the names are dropped and copied by the same code either way.
- A `var` String holds a view when the scan finds it isn't changed in place, read by a closure or taken apart by a pattern (CLX-310). Each value it's given that's `x.copy()`, or a part of `x`, is a view, `cx_str_view`, when `x` is a String in a local, through fields and list, array and map elements, that isn't a `var` or `mut`, is never moved, changed or assigned to in the function, and was in scope where the var was declared, so it outlives the var. A part of the var itself is `cx_string_part_of`, a view while the var's capacity marks one, or a literal's, and a copy while it owns its bytes, which the assignment then frees. Where the var's name moves, it's `cx_string_owned`, which copies a view's bytes into a string of its own and passes any other through. A var given no such view is emitted as before. In the log analysis benchmark, the path each line keeps in a `var` was a copy a line, and another for a path with a query, and is none, about 4% of its time.
- `s.interned()` is `cx_string_interned`, which gives a `cx_String` with a capacity of 0, as a literal's, pointing into the runtime's kept names. Each is a byte with its length, then the text, in 16 KiB chunks from `malloc`, found by a table behind a lock. In front of the table, each thread keeps the last name it met at each of 128 places, a fixed array, so a thread that ends leaves nothing behind. A debug build's count of blocks never freed leaves the kept names out, since they're meant to stay.
- A one-byte separator and the line breaks are found sixteen bytes a step with SSE2 or NEON, inline, so a short part costs no call, as `memchr`'s would. A longer separator goes through `find`.
- `split`, `lines` and `split_whitespace` that make a list keep the first 16 parts on the stack and allocate the list once, at the size it needs, or grow it as `push` does past 16. Each part is a copy, or a view where the parts are kept as views. `cx_text_split_first` stops after the parts a list only indexed by number needs (CLX-309). A list's `join` works out the length first and allocates exactly that once. `s.lines().join(t)` and the others with no list start with room for all of `s`.
- `replace` finds each match with `find` and copies the text between matches with `memcpy`, into a string that starts with room for all of `s`. `repeat` copies what it has so far, doubling, so `n` copies take about `log2(n)` calls.
- They're in [runtime/calx_text.c](../../runtime/calx_text.c), linked only into programs that use them.

Taking a text of 200,000 CSV lines (10.4 MB) apart, in ms, best of 7, from [`bench/text`](../../bench/text), on an i9-13900K pinned to one P-core: each line's fields trimmed and their lengths summed, in loops over `lines()` and `split(",")`; the lines joined with `|`; and that text with `name` replaced by `NAME`:

| | Calx | Rust | Python 3.14 |
| --- | --- | --- | --- |
| The fields of every line, trimmed | 3.9 | 9.8 | 107 |
| The lines joined | 1.2 | 5.8 | 15 |
| A word replaced | 1.7 | 3.7 | 5.9 |

All three give the same total. The Rust is written as Rust usually is: `text.lines()`, `line.split(',')` and `c.trim()`, then `lines().collect::<Vec<_>>().join("|")` and `replace`. Its `lines` and `split` look for the break with a general search that's slower on short parts, and its `join` makes a list of the lines first, which Calx's doesn't.
