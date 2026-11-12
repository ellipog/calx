---
title: std.regex
description: Regular expressions in Rust's syntax, found, captured, replaced and split in time linear in the text, whatever the pattern, with Unicode classes and case folding; and Python's re as it is, with Regex.python.
---

# `std.regex`

Regular expressions, as scripts use Python's `re` and Rust's `regex`: matches found, captured, replaced and counted, and text split by them. The syntax is Rust's. Every search takes time in proportion to the text's length times the pattern's, whatever the two are, so no pattern can hang a program on hostile text. That's the promise RE2, Go and Rust make, and it's why there's no lookaround and no backreferences, which can't keep it.

[`Regex.python`](#pythons-re) is the other way: Python's `re` as CPython 3.14 has it, with its syntax, its lookaround and backreferences, and the matches, groups, templates and errors Python gives, for scripts that move from Python and patterns written for it.

```calx
import std.regex.{Regex, RegexError}

fn main() ! RegexError {
    var date = try Regex.new(#"(?P<y>\d{4})-(?P<m>\d{2})-(?P<d>\d{2})"#)
    let log = "left 2026-11-07, back 2026-12-01"
    println("{date.is_match(log)} {date.count(log)}")    // out: true 2
    if let m = date.find(log) {
        println("{m.start}..{m.end} {m.text}")    // out: 5..15 2026-11-07
    }
    if let c = date.captures(log) {
        println("{c.name("y")} {c.get(2)} {c.span(3)}")    // out: some(2026) some(11) some((13, 15))
    }
    println(date.replace_all(log, #"${d}/${m}/${y}"#))    // out: left 07/11/2026, back 01/12/2026

    var words = try Regex.new(#"\b\w+ing\b"#)
    for m in words.find_all("building and mining, not inner") {
        println(m.text)
    }
    // out: building
    // out: mining

    var sep = try Regex.new(#"\s*[,;]\s*"#)
    println(sep.split("a , b;c").join("|"))    // out: a|b|c

    match Regex.new("(a") {
        ok(_) => {},
        err(e) => println("{e}"),    // out: column 1: `(` with no `)` to close it
    }
}
```

A `Regex` keeps what its searches work out for the next one, so one that searches is declared with `var`, and is best made once and used for many texts, not made again for each. Places in the text are byte offsets, as `String`'s slices take them, and always where a rune starts.

## Items

| Item | Gives |
| --- | --- |
| `Regex.new(pattern: String) -> Regex ! RegexError` | The pattern compiled, or why it isn't one |
| `Regex.python(pattern: String) -> Regex ! RegexError` | The pattern compiled as Python's `re.compile` compiles it, or the error it raises. Every method then does what Python's does: see [Python's `re`](#pythons-re) |
| `RegexError` | `message`, and the `column` in the pattern, in runes from 1. `{e}` is `column 4: ...`. For `Regex.python`, an error Python gives no place for has column 0, and `{e}` is the message alone |
| `Match` | `start` and `end`, byte offsets, and `text`, a copy of the matched text |
| `Captures` | A match's groups. `c.get(i)` and `c.name(name)` are a group's text, `c.span(i)` its `(start, end)`, `none` for a group that took no part; `c.groups[i]` is the `Match?`, and `c.len()` the groups and the whole match, which is group 0 |
| `re.is_match(text) -> bool` | Whether there's a match anywhere |
| `re.find(text) -> Match?` | The first match, as Python's `re.search` finds it |
| `re.find_at(text, from: usize) -> Match?` | The first match that starts at byte `from` or after. `\b` and `^` see the text before `from`, so they mean what they would in the whole text |
| `re.find_all(text) -> List<Match>` | Every match, one after another, none overlapping |
| `re.count(text) -> usize` | How many `find_all` finds, without making them or copying their text |
| `re.captures(text) -> Captures?`, `re.captures_at(text, from)` | The first match and its groups |
| `re.captures_all(text) -> List<Captures>` | Every match and its groups |
| `re.match_start(text) -> Match?`, `re.captures_start(text) -> Captures?` | The match at the start of the text, if one starts there, as Python's `re.match` finds it: the match `find` would give if matches could only start there, so `a\|ab` gives the `a` of `ab` |
| `re.full_match(text) -> Match?`, `re.full_captures(text) -> Captures?` | The match that's the whole text, if there's one, as Python's `re.fullmatch` finds it: the way through the pattern it prefers of those that end at the end, so `a\|ab` matches all of `ab` |
| `re.replace(text, with) -> String`, `re.replace_all(text, with)` | The text with its first match, or every match, replaced by the template `with`. See [Templates](#templates) |
| `re.replace_with(text, f) -> String`, `re.replace_all_with(text, f)` | The same with each match replaced by what the closure `f` gives for its `Captures`, as Python's `re.sub` with a function. `f` is called on the matches in order and can change what it captures, such as a count |
| `re.split(text) -> List<String>` | The parts between the matches, empty ones too, as Python's `re.split` gives them without groups |
| `re.pattern()`, `{re}` | The pattern as it was written |
| `re.group_count() -> usize` | The groups, not counting the whole match |
| `re.group_names() -> List<String>` | Each group's name, in order, `""` for one without |

```calx
import std.regex.{Regex, RegexError}

fn main() ! RegexError {
    var id = try Regex.new("[a-z_]+:[a-z0-9_/]+")
    println("{id.full_match("minecraft:stone") != none} {id.full_match("minecraft:stone!") != none} {id.match_start("minecraft:stone!")!.end}")    // out: true false 15
    var alt = try Regex.new("a|ab")
    println("{alt.match_start("ab")!.text} {alt.full_match("ab")!.text} {alt.match_start("ba") == none}")    // out: a ab true
    var date = try Regex.new(#"(\d{4})-(\d{2})"#)
    let c = date.full_captures("2026-11")!
    println("{c.get(1) ?? ""} {c.get(2) ?? ""}")    // out: 2026 11
}
```

## Syntax

The syntax is Rust's `regex`, which is Python's `re` and Perl's without the parts that can't be searched in linear time.

| Pattern | Matches |
| --- | --- |
| `a`, `\.`, `\x41`, `\u{1F600}` | That rune. `\xHH`, `\uHHHH` and `\UHHHHHHHH` take that many hex digits, or any number in braces |
| `.` | Any rune but `\n`, or any rune at all with `(?s)` |
| `[abc]`, `[a-z]`, `[^a-z]` | A rune in the class, or not in it. Classes nest, `[a-c[x-z]]`, and hold the escapes below |
| `[[:alpha:]]` | The POSIX classes, ASCII only: `alnum`, `alpha`, `ascii`, `blank`, `cntrl`, `digit`, `graph`, `lower`, `print`, `punct`, `space`, `upper`, `word` and `xdigit`, and `[[:^alpha:]]` for the rest |
| `\d`, `\w`, `\s` | A Unicode digit (Nd), word rune (letters, marks, digits, connector punctuation and the joiners) or white space; `\D`, `\W` and `\S` the rest. ASCII only with `(?a)` |
| `xy`, `x\|y` | `x` then `y`; `x`, or else `y` |
| `x*`, `x+`, `x?` | Any number, at least one, at most one, as many as can be |
| `x{n}`, `x{n,}`, `x{n,m}`, `x{,m}` | `n` times; at least `n`; from `n` to `m`; at most `m` |
| `x*?`, `x+?`, `x??`, `x{n,m}?` | The same, as few as can be |
| `(x)`, `(?P<name>x)`, `(?<name>x)` | A group, numbered from 1 in the order the `(`s come, and one with a name too |
| `(?:x)`, `(?i:x)` | A group that doesn't capture, and one with flags of its own |
| `^`, `$` | The start and end of the text, or of a line with `(?m)` |
| `\A`, `\z` | The start and end of the text, always |
| `\b`, `\B` | A place between a word rune and something else, and a place that isn't one |

The flags, set for the rest of the group with `(?i)` or turned off with `(?-i)`:

| Flag | Means |
| --- | --- |
| `i` | Case doesn't matter, as Unicode's simple case folding has it: `(?i)k` matches `K` and the Kelvin sign, and `(?i)ß` matches `ẞ`, but not `ss` |
| `m` | `^` and `$` match at each line's start and end |
| `s` | `.` matches `\n` too |
| `x` | Spaces and `#` comments in the pattern are ignored, outside classes; `\ ` is a space |
| `U` | `*` and the rest take as few as can be, and `*?` as many |
| `a` | `\d`, `\w`, `\s` and `\b` are ASCII only |

Where it differs from Python's `re`:

- `$` is the end of the text, or with `(?m)` of a line. Python's `$` also matches before a `\n` that ends the text; write `\n?$` for that.
- A quantifier after another is an error, so `a**` and `a{2}*` need a group: `(?:a{2})*`. `x{,m}` is at most `m`, as in Python, though Rust refuses it.
- `{` that doesn't start a count is the character, as in Python: `a{x`.
- Lookaround, `(?=x)` and the rest, backreferences, `\1` and `(?P=name)`, atomic groups and possessive quantifiers are errors that say why. Unicode properties, `\p{L}`, are an error for now.
- Classes are Unicode by default, as in Python 3 and Rust; `(?a)` is ASCII, as `re.ASCII` is.
- Case folding is Unicode's simple folding, rune for rune, as in Rust, so `(?i)ß` doesn't match `ss`.
- A repeat whose body can match nothing doesn't take an empty pass after one that matched something, as in Rust, so `(a?)+` matching `a` leaves group 1 as `a`, where Python's takes one more pass and leaves it empty.

Which match is found is as Python and Rust choose: the leftmost, and of the matches that start there, the one the pattern prefers, trying alternatives in order and quantifiers as many times as they can, or as few for lazy ones. So `sam|samwise` finds `sam` in `samwise`.

Since `{` and `}` interpolate in Calx's strings, patterns with a count, such as `\d{4}`, are written as raw strings: `#"\d{4}"#`, as above.

## Templates

`replace` and `replace_all` put the template in place of each match, with these replaced:

| In the template | Is |
| --- | --- |
| `$1`, `${1}` | Group 1's text, or nothing if it took no part |
| `$name`, `${name}` | The named group's text |
| `$0` | The whole match |
| `$$` | A `$` |

A name or a number after `$` takes as many letters, digits and `_` as follow, so `$1a` is group `1a`; write `${1}a` for group 1 then `a`. Write templates as raw strings too, since `${name}` would interpolate.

When the new text has to be worked out, `replace_all_with` takes a closure instead, which gets each match's `Captures`:

```calx
import std.regex.{Regex, RegexError}

fn main() ! RegexError {
    var stacks = try Regex.new(#"(\d+) (\w+)"#)
    println(stacks.replace_all_with("3 stone, 12 dirt", |c| "{(i64.parse(c.get(1) ?? "0") ?? 0) * 64} {c.get(2) ?? ""}"))    // out: 192 stone, 768 dirt
}
```

## Empty matches

A pattern that can match nothing, such as `a*`, finds empty matches, as Rust does: one after another isn't one, and none is inside a rune. So `a*` finds `0-0` and `1-4` in `baaa`, not an empty match after the `aaa`, and an empty pattern finds the places between the runes of `é1`, `0`, `2` and `3`, not `1`, which is inside `é`. `split` and `replace_all` follow from those matches: `""` replaced by `-` in `ab` is `-a-b-`.

## Errors

`Regex.new` gives a `RegexError` whose `column` is where the trouble is, in runes from 1, and whose message says what's wrong and, where it can, what to write instead:

```calx
import std.regex.{Regex}

fn show(pattern: String) {
    match Regex.new(pattern) {
        ok(_) => println("ok"),
        err(e) => println("{e}"),
    }
}

fn main() {
    show("(?<=a)b")    // out: column 1: lookaround, such as `(?=...)` and `(?<!...)`, isn't supported: std.regex finds every match in linear time, which lookaround can't keep
    show("a**")    // out: column 3: `*` after something that can't be repeated, such as another quantifier; put it in a group to repeat it again
    show("[z-a]")    // out: column 2: `z-a` is a range whose end comes before its start
    show(#"x{3,2}"#)    // out: column 2: `{3,2}` repeats at least 3 times and at most 2
}
```

A count is at most 100000, and a pattern compiles to at most 2000000 instructions: `x{1,100000}` is better written `x+`.

## Python's `re`

`Regex.python(pattern)` compiles the pattern as Python 3.14's `re.compile` does, with Python's syntax: lookahead and lookbehind, backreferences such as `\1` and `(?P=name)`, atomic groups, possessive quantifiers, conditionals such as `(?(1)>|$)`, Python's `$`, and Python's classes and case folding. The `Regex` it gives has the same methods, and each finds what Python's would:

| Method | As Python's |
| --- | --- |
| `find(text)`, `find_at(text, from)`, `is_match(text)`, `captures(text)` | `p.search(text)` and `p.search(text, pos)`, and the groups of the match it finds |
| `find_all(text)`, `captures_all(text)`, `count(text)` | `p.finditer(text)`: an empty match right after a match is one, unless that match was empty too |
| `match_start(text)`, `captures_start(text)`, `full_match(text)`, `full_captures(text)` | `p.match(text)` and `p.fullmatch(text)` |
| `replace(text, with)`, `replace_all(text, with)` | `p.sub(with, text, count=1)` and `p.sub(with, text)`, with Python's templates |
| `replace_with(text, f)`, `replace_all_with(text, f)` | `p.sub(f, text, count=1)` and `p.sub(f, text)` with a function |
| `split(text)` | `p.split(text)`, with each group's text after each part |

```calx
import std.regex.{Regex, RegexError}

fn main() ! RegexError {
    // A backreference: words that end with the letter they start with.
    var same = try Regex.python(#"\b(\w)\w*\1\b"#)
    for m in same.find_all("level noon test radar abc") {
        println(m.text)
    }
    // out: level
    // out: noon
    // out: test
    // out: radar

    // Lookbehind, and a template as re.sub reads it.
    var price = try Regex.python(#"(?<=\$)(\d+)\.(\d{2})"#)
    println(price.replace_all("tea $3.50, cake $12.05", #"\1,\2"#))    // out: tea $3,50, cake $12,05

    // Python's empty matches, and split with its groups.
    var star = try Regex.python("x*")
    println(star.replace_all("abxd", "-"))    // out: -a-b--d-
    var sep = try Regex.python(#"\s*([,;])\s*"#)
    println(sep.split("a , b;c").join("|"))    // out: a|,|b|;|c

    // A rune by its name, in any case, or by an alias.
    var dash = try Regex.python(#"\s*[\N{em dash}\N{EN DASH}]\s*"#)
    println(dash.replace_all("1990 – 1999 — the end", "-"))    // out: 1990-1999-the end

    // Python's errors, at Python's places.
    match Regex.python("(?P<1x>a)") {
        ok(_) => {},
        err(e) => println("{e}"),    // out: column 5: bad character in group name '1x'
    }
    match Regex.python(#"\N{LATIN SMALL LETTER Q WITH TAIL}"#) {
        ok(_) => {},
        err(e) => println("{e}"),    // out: column 1: undefined character name 'LATIN SMALL LETTER Q WITH TAIL'
    }
}
```

A template is Python's: `\1` to `\99` and `\g<1>` are a group by number, `\g<name>` by name and `\g<0>` the whole match; `\n`, `\t` and Python's other escapes are what they are, `\0` and `\101` are octal, and a `\` before something else that isn't a letter stays. A group that took no part is empty there. A template Python refuses, such as `\q` or `\3` for a pattern with two groups, panics with the error Python raises, at the call.

Where it differs from Python:

- Places in the text are byte offsets into its UTF-8, as everywhere in Calx, where Python counts code points. The matches are the same text.
- Flags are written in the pattern, `(?i)`, `(?m)`, `(?s)`, `(?x)`, `(?a)` and the rest, as Python takes them inline: `re.compile(p, re.I | re.M)` is `Regex.python("(?im){p}")`.
- `split` gives `""` for a group that took no part, where Python gives `None`.
- There are no bytes patterns and no `re.LOCALE`.
- `match` is a keyword in Calx, so `re.match` is `match_start`.

It matches by backtracking, as Python does, so it isn't linear: a pattern such as `(a+)+b` takes time exponential in a run of `a`s with no `b`, there as here. For patterns that come from outside a program, `Regex.new` is the one that can't hang.

## How it works

`std.regex` is written in Calx: [std/regex.calx](../../std/regex.calx), with the Unicode tables in [std/text/ucd.calx](../../std/text/ucd.calx) and the files of numbers it embeds, which [tools/unicode/gen_ucd.calx](../../tools/unicode/gen_ucd.calx) writes from the Unicode Character Database's own files, version 16.0.0.

- The parser turns the pattern into a tree, with classes as sorted ranges of runes and `(?i)` literals as the runes they fold to.
- The tree is compiled twice. Once to a program of runes for a Pike VM, which runs every thread of the pattern in step, one rune at a time, and so finds the groups in linear time. And once to a program of bytes, with each class turned into the UTF-8 sequences of its runes, for two lazy DFAs: one reads forward and finds where the leftmost-first match ends, and one reads back from there and finds where it starts. The DFAs' states are worked out the first time a search needs them and kept, up to 8 MB of transitions, after which they're worked out again. A state is a row of transitions, one for each class of bytes the pattern can't tell apart, so a byte costs a lookup and a load.
- A search runs the DFAs, then, only if it wants the groups, a bounded backtracker over the match alone, as Rust's regex has: depth-first in the order the pattern prefers, so the first match it reaches is the leftmost-first one, with a bit for each instruction at each place so none is tried twice, which keeps it linear. A match too long for 256 KB of those bits goes to the Pike VM instead. `find` and `count` run neither, and `replace_all` reads its template once and fills it from the groups' places, finding the groups only when the template names one. A DFA that can't decide gives up and the Pike VM searches instead: that's only when Unicode `\b` meets a byte that isn't ASCII, which the DFAs don't judge.
- Before the DFAs read a byte, the search skips to where a match can be. A pattern that's only a literal is found as `String.find` finds text. A pattern every match of which starts with a literal, such as `(?m)^player\d+`, is found at each place the literal is, where a search anchored there finds the match's end. One whose matches start with one of a few bytes, such as `(?i)steve`, skips to the next of those bytes whenever the DFA is back to looking for a start. One with a literal inside every match, such as the `-` in `\d{4}-\d{2}`, is found from the literal: the DFA for the part before it reads back from each place it is, and the forward DFA reads on from where that part starts. When the part before can take the literal's first byte, as `\w+` can in `\b\w+ing\b`, the search skips ahead instead to the earliest a match with the literal's next place can start, the byte after the last one before it that the part can't take. Skips that turn out shorter than 8 bytes on average stop, and a search from a literal that would read the same bytes again gives up and starts over without it, so a skip never makes a search slower than reading every byte.
- Finding text, in `String.find` and these skips, tests 32 places at a time with AVX2, or 16 with SSE2 or NEON, for the needle's two rarest bytes, and compares the rest only where both are.
- A pattern whose matches all start with one of up to 8 strings of 2 to 8 bytes skips to the next place one of them is, when a skip by its first bytes wouldn't go far. `agggtaaa|tttaccct` starts with `a` or `t` at half the places in DNA, but with one of those two strings at one in 32,000. Each step tests 32 or 16 places for the strings' first 4 bytes at once, and compares the whole of each only where one passes (CLX-297). A pattern that's nothing but such strings of one length, with no groups, assertions or repeats, such as `[cgt]gggtaaa|tttaccc[acg]` or `a[NSt]|BY`, needs no DFA at all: only one of them can be at a place, so the place found is the match. And a pattern whose matches all start with one byte, such as `<[^>]*>`, or with one of those strings, is found at each place the skip goes, by a search anchored there, as for a literal prefix, so each match is read once rather than forward and back.

On the Benchmarks Game's regex-redux over fasta n = 1 million, 10 MB, on an i9-13900K under Windows, those took Calx from 191 ms to 106, where Rust's regex crate takes 134. Counting the nine motifs went from 80 ms to 21, and the five replacements from 112 to 63.

`Regex.python` is [std/regex/sre.calx](../../std/regex/sre.calx), which follows CPython's own `re`, under the Python Software Foundation's license, whose notice it keeps:

- The parser is `re/_parser.py`'s, with its errors and their places, and the compiler `re/_compiler.py`'s, to sre's opcodes, numbered as sre numbers them, with Python's case folding from `re/_casefix.py` and its classes: `\d` is Unicode's decimal digits, `\s` what `str.isspace` takes and `\w` what `str.isalnum` takes and `_`. Python's lower and upper case, white space and printable runes are tables in std/text/ucd.calx, from the same Unicode Character Database. `\N{name}` is looked up as `unicodedata.lookup` looks it up, by [std.unicode.names](unicode-names.md), with `lookup_rune`, whose tables add 347 KB to a program that uses std.regex: on Windows such a program is 740 KB in a release build.
- The matcher is a backtracking machine that takes the steps `Modules/_sre/sre_lib.h` takes, with the same marks for the groups, the same protection against repeating an empty match, and the same order of trying, so what it finds and what each group holds are Python's. It keeps its own stack rather than recursing, so a deep pattern can't overflow the program's.
- A search skips to where a match can start, as sre's INFO block has it and further. A pattern every match of which starts with a literal is found at each place the literal is, as `String.find` finds text, and one that's only a literal needs no matching at all. Otherwise the bytes a match can start with are worked out from the pattern, past groups, lookaround and assertions, into branches and through case-insensitive literals, and the search skips to the next of them. Only patterns that can't match nothing skip, so Python's rule for empty matches is never passed over.
- A repeat of one rune, such as `\w+`, `\d{4}` or `.*`, takes its runes in a loop of its own kind ([CLX-225](https://acta.aaenz.no/aaen-studios/clx#CLX-225)). `.` finds the line's end as a byte and counts the runes before it, testing none. A class tests an ASCII byte against a bitmap of the ASCII runes it holds, made when it's compiled, and decodes only the rest; it does so wherever it's tested.
- It was checked against CPython 3.14's `re` on tens of thousands of random patterns, with texts of Python's tricky runes, for the same matches, groups, `finditer`, `split` and `sub` results and the same errors at the same places.

On our benchmark, 200,000 lines of a server's log, 17 MB, on an i9-13900K's performance cores, the best of 3 to 5 runs, against Python 3.14's `re`. Where Python counts `finditer`'s matches, Calx makes `captures_all`, which copies each match's text and groups, except in the first row:

| Pattern | Calx | Python |
| --- | --- | --- |
| `(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2}) \[(\w+)\] (\w+): (.*)`, matches counted with `count` | 44 ms | 64 ms |
| The same, every group read | 79 ms | 106 ms |
| `\b\w+ing\b` | 185 ms | 367 ms |
| `(?i)error\|warning` | 58 ms | 192 ms |
| `\s+`, each replaced by a space | 73 ms | 359 ms |
| `(\w)\1` | 200 ms | 319 ms |
| `(?<=id=)\d+` | 63 ms | 115 ms |
| `Steve` | 5.2 ms | 7.9 ms |
| `[\[\]:]`, split | 28 ms | 120 ms |
