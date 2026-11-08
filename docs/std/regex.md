---
title: std.regex
description: Regular expressions in Rust's syntax, found, captured, replaced and split in time linear in the text, whatever the pattern, with Unicode classes and case folding.
---

# `std.regex`

Regular expressions, as scripts use Python's `re` and Rust's `regex`: matches found, captured, replaced and counted, and text split by them. The syntax is Rust's. Every search takes time in proportion to the text's length times the pattern's, whatever the two are, so no pattern can hang a program on hostile text. That's the promise RE2, Go and Rust make, and it's why there's no lookaround and no backreferences, which can't keep it.

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
| `RegexError` | `message`, and the `column` in the pattern, in runes from 1. `{e}` is `column 4: ...` |
| `Match` | `start` and `end`, byte offsets, and `text`, a copy of the matched text |
| `Captures` | A match's groups. `c.get(i)` and `c.name(name)` are a group's text, `c.span(i)` its `(start, end)`, `none` for a group that took no part; `c.groups[i]` is the `Match?`, and `c.len()` the groups and the whole match, which is group 0 |
| `re.is_match(text) -> bool` | Whether there's a match anywhere |
| `re.find(text) -> Match?` | The first match, as Python's `re.search` finds it |
| `re.find_at(text, from: usize) -> Match?` | The first match that starts at byte `from` or after. `\b` and `^` see the text before `from`, so they mean what they would in the whole text |
| `re.find_all(text) -> List<Match>` | Every match, one after another, none overlapping |
| `re.count(text) -> usize` | How many `find_all` finds, without making them or copying their text |
| `re.captures(text) -> Captures?`, `re.captures_at(text, from)` | The first match and its groups |
| `re.captures_all(text) -> List<Captures>` | Every match and its groups |
| `re.replace(text, with) -> String`, `re.replace_all(text, with)` | The text with its first match, or every match, replaced by the template `with`. See [Templates](#templates) |
| `re.split(text) -> List<String>` | The parts between the matches, empty ones too, as Python's `re.split` gives them without groups |
| `re.pattern()`, `{re}` | The pattern as it was written |
| `re.group_count() -> usize` | The groups, not counting the whole match |
| `re.group_names() -> List<String>` | Each group's name, in order, `""` for one without |

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

## How it works

`std.regex` is written in Calx: [std/regex.calx](../../std/regex.calx), with the Unicode tables in [std/text/ucd.calx](../../std/text/ucd.calx), which [tools/unicode/gen_ucd.calx](../../tools/unicode/gen_ucd.calx) writes from the Unicode Character Database's own files, version 16.0.0.

- The parser turns the pattern into a tree, with classes as sorted ranges of runes and `(?i)` literals as the runes they fold to.
- The tree is compiled twice. Once to a program of runes for a Pike VM, which runs every thread of the pattern in step, one rune at a time, and so finds the groups in linear time. And once to a program of bytes, with each class turned into the UTF-8 sequences of its runes, for two lazy DFAs: one reads forward and finds where the leftmost-first match ends, and one reads back from there and finds where it starts. The DFAs' states are worked out the first time a search needs them and kept, up to 8 MB of transitions, after which they're worked out again. A state is a row of transitions, one for each class of bytes the pattern can't tell apart, so a byte costs a lookup and a load.
- A search runs the DFAs, then, only if it wants the groups, the Pike VM over the match alone. `find` and `count` never run it. A DFA that can't decide gives up and the Pike VM searches instead: that's only when Unicode `\b` meets a byte that isn't ASCII, which the DFAs don't judge.
- Before the DFAs read a byte, the search skips to where a match can be. A pattern that's only a literal is found as `String.find` finds text. A pattern every match of which starts with a literal, such as `(?m)^player\d+`, is found at each place the literal is, where a search anchored there finds the match's end. One whose matches start with one of a few bytes, such as `(?i)steve`, skips to the next of those bytes whenever the DFA is back to looking for a start. One with a literal inside every match, such as the `-` in `\d{4}-\d{2}`, is found from the literal: the DFA for the part before it reads back from each place it is, and the forward DFA reads on from where that part starts. When the part before can take the literal's first byte, as `\w+` can in `\b\w+ing\b`, the search skips ahead instead to the earliest a match with the literal's next place can start, the byte after the last one before it that the part can't take. Skips that turn out shorter than 8 bytes on average stop, and a search from a literal that would read the same bytes again gives up and starts over without it, so a skip never makes a search slower than reading every byte.
- Finding text, in `String.find` and these skips, tests 32 places at a time with AVX2, or 16 with SSE2, for the needle's two rarest bytes, and compares the rest only where both are.
