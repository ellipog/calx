---
title: std.textwrap
description: Text wrapped and filled to a width, shortened, dedented and indented, exactly as Python 3.14's textwrap does it, with its settings and defaults.
---

# `std.textwrap`

Text wrapped and filled to a width, shortened to fit one, and dedented and indented, as Python 3.14's `textwrap` does it: lines break at white space and after hyphens between letters, a word longer than a line is broken, and the settings and their defaults are `TextWrapper`'s.

```calx
import std.textwrap.{Wrapper, dedent, fill, shorten, wrap}

fn main() {
    let text = String("The quick brown fox jumps over the lazy dog, a well-known test of fonts.")
    println(fill(text.copy(), 30))
    // out: The quick brown fox jumps over
    // out: the lazy dog, a well-known
    // out: test of fonts.
    println("{wrap(text.copy(), 12)}")    // out: [The quick, brown fox, jumps over, the lazy, dog, a well-, known test, of fonts.]
    println(shorten(text.copy(), 32))    // out: The quick brown fox jumps [...]
    let usage = Wrapper { width: 32, initial_indent: "  -v  ", subsequent_indent: "      " }
    println(usage.fill(String("Print each file's name as it's read, and how long it took.")))
    // out:   -v  Print each file's name as
    // out:       it's read, and how long it
    // out:       took.
    print(dedent(String("    def f():\n        return 1\n")))
    // out: def f():
    // out:     return 1
}
```

## Items

| Item | Gives |
| --- | --- |
| `wrap(text: String, width: usize = 70) -> List<String>` | `text` as lines of at most `width` runes. Python's `textwrap.wrap(text, width)` |
| `fill(text: String, width: usize = 70) -> String` | `wrap`'s lines joined with `\n`. Python's `textwrap.fill` |
| `shorten(text: String, width: usize, placeholder: String = " [...]") -> String` | `text` with its white space collapsed to single spaces, and if that's longer than `width`, the words that fit and then `placeholder`. Python's `textwrap.shorten` |
| `dedent(text: String) -> String` | `text` without the spaces and tabs every line starts with. Lines of white space only don't count, and become empty. Python's `textwrap.dedent` |
| `indent(text: String, prefix: String) -> String` | `text` with `prefix` before each line that isn't white space only. Python's `textwrap.indent(text, prefix)` |
| `indent_where(text: String, prefix: String, keep: Fn(String) -> bool) -> String` | `text` with `prefix` before each line `keep` is true for, given the line with its end: `\|_\| true` indents every line. Python's `textwrap.indent(text, prefix, predicate)` |
| `Wrapper` | The settings, as Python's `TextWrapper` has them, with `w.wrap(text)`, `w.fill(text)` and `w.shorten(text)` |

`wrap`, `fill` and `shorten` panic for a `width` of 0, and a `Wrapper` with `max_lines` panics if its placeholder doesn't fit on a line, where Python raises `ValueError`. Widths count runes, as Python counts characters, so a CJK character, which a terminal shows two columns wide, counts as one.

## Settings

A `Wrapper` is written with the settings that differ from their defaults, as Python's keyword arguments are: `Wrapper { width: 40, break_long_words: false }`.

| `Wrapper` field | Default | Is |
| --- | --- | --- |
| `width` | `70` | The most runes on a line, its indent too |
| `initial_indent` | `""` | What the first line starts with |
| `subsequent_indent` | `""` | What each line after it starts with |
| `expand_tabs` | `true` | Whether tabs become spaces first, up to the next multiple of `tabsize` on their line |
| `tabsize` | `8` | |
| `replace_whitespace` | `true` | Whether each tab, newline, vertical tab, form feed and carriage return left becomes a space |
| `fix_sentence_endings` | `false` | Whether a sentence's end, a lowercase letter, then `.`, `!` or `?`, and a quote if there's one, gets two spaces after it. It takes `Mr.` and `e.g.` for ends too, as Python does |
| `break_long_words` | `true` | Whether a word longer than a line is broken; otherwise it's a line of its own, too long |
| `drop_whitespace` | `true` | Whether white space at the start and end of each line is dropped. White space at the start of the text stays, if the first line has more than that |
| `break_on_hyphens` | `true` | Whether a line can end after a hyphen between letters, as in `well-` and `known` |
| `max_lines` | `none` | The most lines. The last one shown ends with `placeholder`, if there's more text than fits |
| `placeholder` | `" [...]"` | |

## How lines break

The text is split into chunks, which lines are made of, as Python's pattern splits it:

- **White space**: a run of spaces, tabs and the other ASCII white space, which a line can break at. Other white space, such as the no-break space, is part of the word it's in.
- **Words**: what's between white space, broken after a hyphen with two letters before it, or a letter, a hyphen and a letter, and letters after it, so `state-of-the-art` can break after each hyphen but `x-1` and `-lead` can't. A letter is any rune Python's `\w` matches that isn't a decimal digit.
- **Em dashes**: two hyphens or more, after a word or punctuation and before a word, are a chunk of their own: `word--word` breaks before or after the dashes.

Each line takes as many chunks as fit. A word longer than a whole line is broken where the line ends, after its last hyphen that fits if there's one with more than hyphens before it.

```calx
import std.textwrap.{Wrapper, wrap}

fn main() {
    println("{wrap(String("a state-of-the-art word--word x-1"), 9)}")    // out: [a state-, of-the-, art word, --word, x-1]
    println("{wrap(String("supercalifragilistic"), 8)}")    // out: [supercal, ifragili, stic]
    let w = Wrapper { width: 8, break_long_words: false }
    println("{w.wrap(String("supercalifragilistic is long"))}")    // out: [supercalifragilistic, is long]
    let most = Wrapper { width: 20, max_lines: some(2), placeholder: "..." }
    println("{most.wrap(String("The quick brown fox jumps over the lazy dog."))}")    // out: [The quick brown fox, jumps over the...]
}
```

## Where it differs

Python's `textwrap` never returns when an indent is wider than the line and a word's last rune is split off onto a line of its own: what's left is an empty chunk, which it splits again and again. `wrap(String("  a"), ...)` with a `width` of 2 and an `initial_indent` of 4 spaces is one such. std.textwrap goes on past the empty chunk, and gives `["    a"]`.

## Checked against Python

It matches Python 3.14's `textwrap` on 220,000 random cases: `wrap` with random settings, `shorten`, `dedent` and `indent`, on text of words with hyphens and em dashes, letters, digits and marks from several scripts, and every kind of white space, the ones Python breaks at and the ones it doesn't. The cases Python never finishes, which the test catches as they start, were left out.

Python's `\w` is `str.isalnum` and `_`, which isn't Unicode's own word class: it has no combining marks, and it has every rune with a numeric value, such as `²` and `Ⅳ`. Its runes come from [std/text/ucd.calx](../../std/text/ucd.calx), which [tools/unicode/gen_ucd.calx](../../tools/unicode/gen_ucd.calx) writes from Unicode 16's UnicodeData.txt, the version Python 3.14 has.

## Speed

On our benchmark, an i9-13900K with a little other work running, each program on its performance cores, the best of 20 to 25 runs taken in turns (15 for Python). The prose is 5.4 MB in 9,000 paragraphs, and the code 2.3 MB of indented lines. The comparisons are Python 3.14's `textwrap`, and Rust 1.91 with the `textwrap` crate 0.16.4, built for release with `x86-64-v3`. Rust's defaults choose where lines break to make them even and break at Unicode's line break opportunities, so it was also run with Python's rules: each line as full as it goes, breaking at spaces and after hyphens.

| Work | Calx | Python | Rust, Python's rules | Rust, defaults |
| --- | --- | --- | --- | --- |
| Each paragraph filled to 70 | 39.0 ms | 553 ms | 58.1 ms | 172 ms |
| The whole prose wrapped to 70 | 44.0 ms | 572 ms | 58.2 ms | 170 ms |
| Each paragraph shortened to 60 | 32.3 ms | 463 ms | | |
| The code dedented | 4.1 ms | 14.2 ms | 5.1 ms | |
| The code indented | 2.0 ms | 8.2 ms | 2.7 ms | |

Rust's `wrap` keeps the text's newlines, where Python's and Calx's make them spaces first, so it gives more lines for the whole prose, 136,668 to 78,292.

`indent` finds the bytes that could start a line break 16 at a time with SSE2 or NEON, and looks only at those: a control character, or the first byte of U+0085, U+2028 and U+2029. That took it from 2.6 ms to 2.0 ms, and it matched Python's `indent` on 6,000 more random texts with every kind of line break.
