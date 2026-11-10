---
title: std.unicode.names
description: Unicode 16's character names, as Python 3.14's unicodedata.name and lookup give them, and as Java 25's Character.getName and codePointOf do - name, lookup, java_name and java_code_point_of.
---

# `std.unicode.names`

Unicode 16's character names: a rune's name and the runes a name stands for, as Python 3.14's `unicodedata.name` and `unicodedata.lookup` give them, and as Java 25's `Character.getName` and `Character.codePointOf` do. [std.regex](regex.md)'s `Regex.python` reads `\N{...}` with `lookup`, as Python's `re` does, and [std.nbt](nbt.md) reads SNBT's with the Java ones, as vanilla does.

```calx
import std.unicode.names.{java_code_point_of, java_name, lookup, name}

fn main() {
    println(name('é') ?? String("none"))    // out: LATIN SMALL LETTER E WITH ACUTE
    println("{name('\u{AC00}') ?? String()} / {name('\u{4E00}') ?? String()}")    // out: HANGUL SYLLABLE GA / CJK UNIFIED IDEOGRAPH-4E00
    println("{name('\u{E000}') == none} {lookup("snowman") ?? String()} {lookup("NBSP") == some(String("\u{A0}"))}")    // out: true ☃ true
    println(lookup("KEYCAP DIGIT ONE") ?? String())    // out: 1️⃣
    println("{java_name(0xA) ?? String()} / {java_name(0x4E00) ?? String()}")    // out: LINE FEED (LF) / CJK UNIFIED IDEOGRAPHS 4E00
    println("{java_code_point_of(String(" line feed (lf) ")) ?? 0} {java_code_point_of(String("LF")) == none}")    // out: 10 true
}
```

| Function | Gives |
| --- | --- |
| `name(r: rune) -> String?` | The name Python's `unicodedata.name(r)` gives, or `none` where it has none |
| `lookup(text: String) -> String?` | The runes `unicodedata.lookup(text)` gives: one, or several for a named sequence, or `none` where it raises `KeyError` |
| `java_name(c: u32) -> String?` | The name Java's `Character.getName(c)` gives, for any code point, surrogates too, or `none` where it gives `null` or throws |
| `java_code_point_of(text: String) -> u32?` | The code point Java's `Character.codePointOf(text)` gives, or `none` where it throws |

## The names

`name` gives each rune's name in UnicodeData.txt, and the names made from a rune's number:

- a Hangul syllable's, from its jamo's short names, as `HANGUL SYLLABLE GAG`;
- a CJK unified ideograph's, as `CJK UNIFIED IDEOGRAPH-4E00`;
- those of the families numbered in UnicodeData.txt, such as `CJK COMPATIBILITY IDEOGRAPH-F900`, `TANGUT COMPONENT-001` and `VARIATION SELECTOR-17`.

It gives `none` for a control character, a private use rune, a noncharacter, a rune not assigned yet, and a Tangut ideograph, which Python 3.14 has no name for, though its `lookup` takes `TANGUT IDEOGRAPH-17000`.

`lookup` takes:

- a name, with ASCII letters in either case and nothing else changed: no spaces are trimmed, and `_` isn't a space;
- each of NameAliases.txt's aliases: corrections, as `PRESENTATION FORM FOR VERTICAL RIGHT WHITE LENTICULAR BRACKET` for the misspelled name, names for the control characters, as `LINE FEED`, and abbreviations, as `LF`, `NBSP` and `BOM`;
- each of the 461 named sequences, as `KEYCAP DIGIT ONE` and `BENGALI LETTER KHINYA`, which give several runes;
- a Hangul syllable's name, read as CPython reads it, with the longest jamo name that fits each time, so `HANGUL SYLLABLE GGA` is U+AE4C;
- a CJK unified ideograph's or a Tangut ideograph's, with its number in hex in either case and no zero in front.

`java_name` names every assigned code point:

- a rune with a name in UnicodeData.txt by that name, as `name` does, and the families too;
- a control character by its Unicode 1.0 name, as `LINE FEED (LF)`, or by its abbreviation where a later rune took that name, as U+0007 is `BEL` since U+1F514 is `BELL`, or else by an alias, as U+0080 is `PADDING CHARACTER`;
- every other one by its block, in capitals with spaces, and its number in hex: `CJK UNIFIED IDEOGRAPHS 4E00`, `HANGUL SYLLABLES AC00`, `HIGH SURROGATES D800`, `PRIVATE USE AREA E000`, and `LATIN 1 SUPPLEMENT 84` for U+0084, which has no name of either kind.

It gives `none` for a code point not assigned yet, a noncharacter, and anything past U+10FFFF, where Java throws.

`java_code_point_of` drops the spaces and control characters at both ends, as Java's `trim` does, and changes the case as Java's `toUpperCase(Locale.ROOT)` does, which makes `ı` an `I`. The text must then be a name `java_name` gives. It takes no aliases, so `LF` isn't one, and `LINE FEED (LF)` is.

## How it's built

The names are in [std/text/names.calx](../../std/text/names.calx) and the file of bytes it embeds, std/text/names.bin, which [tools/unicode/gen_names.calx](../../tools/unicode/gen_names.calx) writes from Unicode 16.0.0's UnicodeData.txt, NameAliases.txt, NamedSequences.txt, Blocks.txt and Jamo.txt. There are 33,550 entries: the names, the aliases and Java's names for the control characters, in alphabetical order. Each is written as numbers of words, of 11,805 words in all, the most used first, so the most used take one byte. An entry shares what it can of the words of the one before it, and gives its rune as the difference from the one before's. Every 8th entry is written whole, and its first 16 bytes are kept apart. For each rune with a name, in order, the tables give its entry, as the difference from the one before's.

Numbers are written in base 256, a byte to a digit, in the 420,932 bytes of names.bin, which the module makes a [constant with `embed`](../reference/variables.md#file-constants). A build never reads them: they go into the program through the assembler. They add about 430 KB to a program, and about 5 ms to a build that reuses its cached files.

- `name` and `java_name` first skip the blocks of 256 runes that hold nothing assigned. Then they find the rune's entry and read it on from the last one written whole.
- `lookup` and `java_code_point_of` find the last entry written whole that isn't past the name, mostly by the first 16 bytes kept for each. Then they read on from it. An entry whose shared words reach past where the one before differed from the name compares the same way, so it's only skipped over. Another is compared from where its shared words end, and no name is built.

`name` and `java_name` were checked against Python 3.14 and Java 25 on every code point, and `lookup` and `java_code_point_of` on 553,364 names:

- every name and alias of both, and every named sequence;
- a fifth of them in lowercase and a twentieth in title case;
- every Tangut ideograph's name and the code points past each end of their ranges;
- a tenth of them changed by one character: dropped, doubled, swapped, replaced, or with spaces and a control character around them.

All four gave the same answers on every one, in debug and release builds.

## Speed

On our benchmark, an i9-13900K pinned to its performance cores and out of power throttling, the best of 8 runs over rounds taken in turn. Python is 3.14's `unicodedata`, and Java is 25.0.5's `Character`, timed after a first round, so its JIT has compiled it:

| Work | Calx | Python | Java |
| --- | --- | --- | --- |
| `name` of every code point | 13.0 ms | 93.9 ms | |
| `lookup` of each of Python's 148,853 names | 22.7 ms | 31.3 ms | |
| `java_name` of every code point | 13.9 ms | | 25.0 ms |
| `java_code_point_of` of each of Java's 294,579 names | 25.2 ms | | 37.2 ms |

- Of the 148,853 names, 97,668 are CJK ideographs' and 11,172 Hangul syllables'. Those take 67 ns and 161 ns each, where Python takes 153 ns and 537 ns. The other 40,013 take 355 ns each, and Python, which keeps its names in a DAWG, 285 ns.
- Most of Java's names are a block's and a number, which `java_code_point_of` reads straight from the number.

The module is [std/unicode/names.calx](../../std/unicode/names.calx).
