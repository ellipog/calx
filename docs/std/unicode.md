---
title: std.unicode
description: Unicode text normalized to NFC, NFD, NFKC or NFKD, as Python's unicodedata.normalize and is_normalized give it, with Unicode 16's tables, and combining classes.
---

# `std.unicode`

Unicode text normalized, as Python's `unicodedata.normalize` and `unicodedata.is_normalized` do it, with Unicode 16's tables, which are Python 3.14's. The same text can be written more than one way: `é` is one rune, U+00E9, or `e` followed by U+0301, the combining acute accent. Two strings that read the same compare equal once both are in the same form.

```calx
import std.unicode.{Form, is_normalized, normalize}

fn main() {
    let typed = String("caf\u{E9}")
    let pasted = String("cafe\u{301}")
    println("{typed == pasted} {normalize(typed.copy(), Form.NFC) == normalize(pasted.copy(), Form.NFC)}")    // out: false true
    println("{normalize(String("\u{FB01}le \u{2460}"), Form.NFKC)} {is_normalized(pasted, Form.NFC)}")    // out: file 1 false
}
```

| Function | Gives |
| --- | --- |
| `normalize(text, form) -> String` | `text` in `form`. Python's `unicodedata.normalize(form, text)` |
| `is_normalized(text, form) -> bool` | Whether `text` is in `form` already. Python's `unicodedata.is_normalized` |
| `combining(r) -> u8` | The canonical combining class of the rune `r`: 0 for a starter, such as a letter, and otherwise the class that orders the marks after it, such as 230 for an accent above and 220 for one below. Python's `unicodedata.combining` |

| `Form` | Is |
| --- | --- |
| `NFC` | Canonical decomposition, then composition: `é` as one rune. What most text is in already, and the form to compare and store text in |
| `NFD` | Canonical decomposition: `é` as `e` and the mark, with marks in their canonical order |
| `NFKC` | Compatibility decomposition, then composition: also `ﬁ` as `fi`, `²` as `2` and `①` as `1`. For matching text as people read it, such as names and identifiers |
| `NFKD` | Compatibility decomposition |

Characters' names, as `unicodedata.name` and `unicodedata.lookup` give them, are in [std.unicode.names](unicode-names.md), a module of their own, so a program that only normalizes doesn't carry them.

Hangul syllables decompose into their jamo and compose back from them, as the Unicode Standard's arithmetic does it. The runes U+0958 and the like, which Unicode excludes from composing, stay decomposed in NFC, as they do in Python.

```calx
import std.unicode.{Form, combining, normalize}

fn codes(s: String) -> String {
    var parts: List<String> = []
    for r in s.runes() {
        parts.push("{u32(r):X}")
    }
    return parts.join(" ")
}

fn main() {
    println(codes(normalize(String("\u{D55C}"), Form.NFD)))    // out: 1112 1161 11AB
    println(codes(normalize(String("\u{1112}\u{1161}\u{11AB}"), Form.NFC)))    // out: D55C
    println(codes(normalize(String("a\u{301}\u{323}"), Form.NFC)))    // out: 1EA1 301
    println("{combining('\u{301}')} {combining('\u{323}')}")    // out: 230 220
}
```

## How it's built

[std/unicode.calx](../../std/unicode.calx) is written in Calx from UAX #15, the standard for normalization. Its tables, files of numbers that [std/text/normal.calx](../../std/text/normal.calx) embeds, so a build never reads them, are written by [tools/unicode/gen_normal.calx](../../tools/unicode/gen_normal.calx) from the Unicode Character Database's own files. For each rune they hold:

- its combining class;
- its full canonical and compatibility decompositions, each applied again to what it gives until nothing decomposes further;
- its quick checks, which say whether it can be in text in each form.

Then the pairs that compose, in a hash table. Each rune's record is found through two stages of blocks of 128 runes, and a byte for each block says in which forms every rune in it passes the quick check.

It passes Unicode 16's NormalizationTest.txt: all 19,965 of its lines in all four forms, and every other rune unchanged in each. It also matches Python 3.14's `normalize` and `is_normalized` in all four forms on 160,000 random strings, which were heavy in marks, Hangul, compatibility forms and runes from every plane, and on 6,000 more made of the runes around the edges of the blocks the quick check skips.

`normalize` first runs the quick check, which skips runs of runes that can't change, 32 bytes at a time with AVX2, as release builds have, and 16 with SSE2 or NEON:

- ASCII, which is in every form;
- in NFC, everything below U+0300, which in UTF-8 is every byte below 0xCC;
- CJK's ideographs, U+4E00 to U+9FFF, in every form;
- Hangul's syllables in NFC and NFKC;
- in NFC, Latin Extended Additional, U+1E00 to U+1EFF, in which most of Vietnamese's letters are.

Other runes in blocks that pass whole are read one at a time. Text that's already in the form is copied as it is. Otherwise the check stops at the first rune that isn't certainly in the form, and backs up to the last starter before it, since nothing before a starter can compose with what's after it. The text before that is copied, and the rest is decomposed into a few pending runes, which are put in canonical order. In NFC and NFKC they're composed and written out at each starter nothing composes into; in NFD and NFKD, starters go straight out. `is_normalized` reads on past a rune that only might not be in the form, since finding one that can't be saves it normalizing.

## Speed

On our benchmark, an i9-13900K pinned to its performance cores and out of power throttling, the best of 32 runs over rounds taken in turn. Python's two decompositions take seconds, so they ran once a round, and are the best of 8. The texts are words picked at random: English, Vietnamese in NFC, the same Vietnamese in NFD, Korean, and text full of compatibility forms. The comparisons are Python 3.14's `unicodedata`, and Rust 1.91 with unicode-normalization 0.1.26, built for release with `x86-64-v3`, collecting its iterator into a `String`, and with its quick check run first:

| Work | Calx | Python | Rust | Rust, quick check first |
| --- | --- | --- | --- | --- |
| English, 9.5 MB, to NFC, which it's in | 0.96 ms | 0.01 ms | 68.1 ms | 4.0 ms |
| Vietnamese, 6.9 MB, to NFC, which it's in | 0.83 ms | 5.5 ms | 64.0 ms | 15.2 ms |
| Vietnamese, to NFD | 25.0 ms | 6,774 ms | 51.0 ms | 50.8 ms |
| Vietnamese in NFD, 8.5 MB, to NFC | 26.7 ms | 114 ms | 61.4 ms | 77.8 ms |
| Korean, 9.1 MB, to NFC, which it's in | 1.24 ms | 4.3 ms | 70.3 ms | 17.8 ms |
| Korean, to NFD | 19.7 ms | 14,032 ms | 59.2 ms | 59.3 ms |
| Compatibility forms, 6.3 MB, to NFKC | 24.3 ms | 2,141 ms | 44.3 ms | 44.0 ms |

- Python's string knows when it's all ASCII, so its `normalize` gives such a string back without reading it. Its other strings hold each character at a fixed width, so its quick check reads them without decoding UTF-8. Its decomposing is much slower: it took seconds on the texts above.
- `normalize` gives back a copy of text that's already in the form, where Python gives back the same string. The copy is most of the English row: the check alone, `is_normalized`, takes 0.14 ms there.
