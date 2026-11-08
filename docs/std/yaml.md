---
title: std.yaml
description: YAML text parsed into a Yaml tree as PyYAML's safe_load reads it, YAML 1.1 and PyYAML's quirks included, with the line and column of any mistake, and trees written back byte for byte as safe_dump writes them.
---

# `std.yaml`

YAML, the format of CI pipelines, Docker Compose, Kubernetes and Ansible: text parsed into a tree of `Yaml` values as Python's PyYAML reads it with `yaml.safe_load`, with the line and column of what's wrong when it isn't YAML, and a tree written back as text, byte for byte as `yaml.safe_dump` writes it. PyYAML reads YAML 1.1, with its quirks, and so does `std.yaml`: `yes` and `off` are bools, `010` is 8, and `1:30` is 90, so a file reads here as it does in Python.

```calx
import std.yaml.{Yaml, YamlError, parse, write}

fn main() ! YamlError {
    let config = try parse(##"
        name: lobby
        network:
          port: 25565
          motd: Welcome
        servers:
        - name: eu west
          enabled: yes
        "##)
    println("{config.get("name")} {config.path("network.port")} {config.path("servers.0.enabled")}")    // out: some(lobby) some(25565) some(true)
    let port = (config.path("network.port") ?? Yaml.Int(25565)).as_i64() ?? 25565
    println("{port + 1}")    // out: 25566
    print(write(config))
    // out: name: lobby
    // out: network:
    // out:   port: 25565
    // out:   motd: Welcome
    // out: servers:
    // out: - name: eu west
    // out:   enabled: true
    match parse(String("port: 25565\n  motd: hi")) {
        ok(_) => {},
        err(e) => println("{e}"),    // out: line 2, column 7: a `:` where a value can't start; quote text that holds `: `
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `Yaml` | A YAML value. See [Values](#values) |
| `YamlError` | Why text isn't YAML as PyYAML reads it: `message`, and the `line` and `column` in the text, from 1. `{e}` is `line 3, column 5: ...` |
| `parse(text: String) -> Yaml ! YamlError` | The document in `text`, as `yaml.safe_load(text)` reads it: `Yaml.Null` for no document, and an error for a second one |
| `parse_all(text: String) -> List<Yaml> ! YamlError` | Every document in `text`, as `list(yaml.safe_load_all(text))` reads them |
| `write(doc: Yaml, ...) -> String` | `doc` as a document, as `yaml.safe_dump(doc, sort_keys=False, allow_unicode=True)` writes it. See [Writing](#writing) for what else it takes |
| `write_all(docs: List<Yaml>, ...) -> String` | The documents, as `yaml.safe_dump_all` writes them, with `---` before each but the first. It takes what `write` takes |
| `{v}` | The value in flow style on one line: `{a: 1, b: [x, 'yes']}` |
| `v.get(key: String) -> Yaml?` | A copy of the string key `key`'s value, if `v` is a mapping that has it |
| `v.at(i: usize) -> Yaml?` | A copy of element `i`, if `v` is a list that long |
| `v.path(key: String) -> Yaml?` | A copy of the value the keys of `key` name in turn, between dots: `network.port`, a number for a list's element, `servers.0.name`, and quotes around a key with a dot in it, `hosts."eu.west".port`. A number names an integer key too where there's no string key like it. `none` if something on the way isn't there. Only what's found is copied |
| `v.as_str()`, `v.as_i64()`, `v.as_bool()`, `v.as_bytes()` | The text, the integer, the bool or the bytes, if `v` is one |
| `v.as_f64() -> f64?` | A float, or an integer as the nearest `f64` |
| `v.as_moment() -> Moment?` | The date and time, if `v` is a date or a date and time. [`Moment`](toml.md#dates-and-times) is std.toml's |
| `v.is_null() -> bool` | Whether `v` is `null` |
| `v.len() -> usize` | A list's elements or a mapping's keys, and 0 for anything else |
| `v.keys() -> List<Yaml>` | A mapping's keys, in order |
| `v.kind() -> String` | `null`, `a bool`, `an integer`, `a float`, `a string`, `bytes`, `a date`, `a date and time`, `a date and time with an offset`, `a list` or `a mapping`, for messages |

A `match` reads a value in place, without the copy that `get` makes:

```calx
import std.yaml.{Yaml, parse}

fn main() {
    let doc = parse(String("deps: [serde, rand]\npackage: \{name: calx\}"))!
    match doc {
        Yaml.Map(m) => {
            for (k, v) in m {
                println("{k}: {v.kind()} {v.len()}")
            }
        },
        _ => {},
    }
    // out: deps: a list 2
    // out: package: a mapping 1
}
```

## Values

| `Yaml` | Is | PyYAML gives |
| --- | --- | --- |
| `Null` | `null`, `Null`, `NULL`, `~`, or nothing | `None` |
| `Bool(bool)` | `true`, `yes`, `on`, `false`, `no` or `off`, in lower case, capitalized or upper case | `bool` |
| `Int(i64)` | `12`, `+12`, `0x1F`, `0b101`, `017` for octal, `1_000`, and `1:30` for base 60, which is 90 | `int` |
| `Float(f64)` | `1.5`, `.5`, `1.`, `1.0e+5` with its point and sign, `1:30.5`, `.inf`, `-.inf` and `.nan` | `float` |
| `Str(String)` | Any other scalar, and any quoted one | `str` |
| `Bytes(List<u8>)` | `!!binary`, base64 in the text | `bytes` |
| `Date(Moment)` | `2026-11-08` | `date` |
| `DateTime(Moment)` | `2026-11-08 14:05:09`, `2026-11-08t14:05:09.5`, with no offset | `datetime` without a `tzinfo` |
| `OffsetDateTime(Moment)` | `2026-11-08T14:05:09+01:00`, `Z` for UTC, or `-5` after a space | `datetime` with a `tzinfo` |
| `Seq(List<Yaml>)` | A list, `- a` or `[a, b]` | `list` |
| `Map(LinkedMap<Yaml, Yaml>)` | A mapping, `a: 1` or `{a: 1}`, its keys in the order the text has them | `dict` |

`==` compares trees as Python compares what PyYAML makes: a `.nan` is equal to a `.nan`, as one object is to itself in Python, and two mappings are equal with the same keys and values, in any order. A tree read back from what `write` wrote is `==` to the one written.

Keys are scalars. A list or a mapping can't be one, as in Python, where they can't be hashed, and keys Python holds equal are one key: `1`, `1.0` and `true` are the same key, as are `0`, `0.0` and `false`, and a later one's value replaces the first's, which keeps its place and its kind.

## Reading

`parse` reads text as PyYAML's pure Python `SafeLoader` does, step for step, as a port of its scanner, parser and resolver:

- **Plain scalars** are what YAML 1.1's patterns say, tried as PyYAML tries them: a bool, an integer, a float, a null, a date or a date and time, or else a string. Quoted scalars are always strings. `0o17` is a string, as YAML 1.1 has no `0o`, and so is `1e3`, which has no point.
- **Block style**: a mapping's keys at one indent, `key: value`, and a list's items at one indent, `- item`, or at the mapping's own indent when the list is a value. A key must be on one line and under 1,024 runes, or come after `? `. Tabs can't indent, as in PyYAML, nor come before a value.
- **Flow style**: `[a, b]` and `{a: 1, b}`, across lines if wanted, with `[a: 1]` a list of one mapping.
- **Block scalars**: `|` keeps the line breaks and `>` folds them into spaces, with `-` to strip the last break, `+` to keep the breaks at the end, and a digit for the indent.
- **Anchors and aliases**: `&name` on a value, and `*name` after it for a copy of it. An anchor can't be given twice in a document, as PyYAML says since 6.0.
- **Merge keys**: `<<: *base` puts the keys of the mapping `base` in, and `<<: [*a, *b]` those of each, the first one's first. The mapping's own keys win.
- **Tags**: `!!str`, `!!int`, `!!float`, `!!bool`, `!!null`, `!!binary`, `!!timestamp`, `!!seq`, `!!map`, `!!set`, `!!omap` and `!!pairs`, as `safe_load` reads them, and `!` for no tag. Any other tag is an error, as in `safe_load`. `%TAG` directives name handles, and `%YAML 1.x` is read.
- **Documents**: `---` starts one and `...` ends one. `parse` reads one, and `parse_all` all of them.

An error gives the line and column of the mistake, and says what's wrong in words, where PyYAML gives short ones, such as `mapping values are not allowed here`:

```calx
import std.yaml.{parse}

fn main() {
    for text in ["a: b: c", "a:\n\t- b", "list: [a, b", "- *nothing", "x: !!int nope", "&a a: &a b"] {
        match parse(String(text)) {
            ok(v) => println("{v}"),
            err(e) => println("{e}"),
        }
    }
    // out: line 1, column 5: a `:` where a value can't start; quote text that holds `: `
    // out: line 2, column 1: a tab, which PyYAML doesn't allow here: indent with spaces, and put spaces before a value
    // out: line 1, column 12: the end in a `[` list, where `,` or `]` should be; the list starts at line 1
    // out: line 1, column 3: `*nothing`, an alias to no anchor before it
    // out: line 1, column 4: `nope` tagged an integer, which it isn't
    // out: line 1, column 7: a second anchor `&a` in the same document
}
```

What reads differently from PyYAML is what Calx can't hold, and is an error instead:

- **An integer past 64 bits**, which Python's `int` holds.
- **An alias inside the node it names**, `&a [*a]`, a list that holds itself, which Python can make but a tree can't.
- **A string with a lone surrogate**, `"\ud800"`, which Python's `str` holds but UTF-8 can't.

And three tags give their nearest tree: `!!set` a mapping with `null` values, where Python has a `set`; and `!!omap` and `!!pairs` a list of two-element lists, where Python has a list of tuples. A date and time keeps nanoseconds, where Python keeps microseconds.

Aliases copy, so text can't make a tree much bigger than itself with aliases of aliases, the "billion laughs": `parse` gives an error when they'd copy more values than a few times the text's length.

## Writing

`write` writes a tree as `yaml.safe_dump(doc, sort_keys=False, allow_unicode=True)` does, byte for byte, as a port of PyYAML's emitter and representer:

- **Block style**, two spaces a level, with a list that's a mapping's value at the mapping's indent, and an empty list or mapping as `[]` or `{}`.
- **Strings** plain where they'd read back as themselves, and quoted where they wouldn't: `'yes'`, `'1.5'`, `''`, `'#tag'`, `' lead'`. A string with a break in it is in single quotes across lines, and one with a control character, or a `\`, in double quotes with escapes.
- **Long lines** folded at the spaces past 80 columns, where the string's style can fold.
- **Floats** as Python's `repr`, `1.0e+16` with a point before the exponent, and `.inf`, `-.inf` and `.nan`; dates as `2026-11-08` and dates and times as `2026-11-08 14:05:09+01:00`, as Python's `isoformat(' ')` writes them; and bytes as `!!binary |` with base64 lines of 76.
- **A document that's a plain scalar** ends with `...`, as PyYAML ends one.

| Option | Default | PyYAML's |
| --- | --- | --- |
| `sort_keys: bool` | `false` | `sort_keys`. Each mapping's keys in order, when they can be compared: all numbers, all strings, all bytes, all dates, or all dates and times of one kind. A mapping with keys that can't be, a string and a number, keeps its order, as in Python |
| `flow: bool` | `false` | `default_flow_style=True`: every list and mapping in flow style |
| `indent: usize` | 2 | `indent`, from 2 to 9 |
| `width: usize` | 80 | `width`, the column to fold at, past twice the indent |
| `ascii: bool` | `false` | `allow_unicode=False`: what isn't ASCII written escaped, in double quotes |
| `explicit_start: bool` | `false` | `explicit_start`: `---` before the first document too |

```calx
import std.yaml.{Yaml, parse, write}

fn main() {
    let doc = parse(String("b: [1, 'yes', 1.0e+16]\na: \"two\\nlines\"\nc: \{\}"))!
    print(write(doc, sort_keys: true))
    // out: a: 'two
    // out:
    // out:   lines'
    // out: b:
    // out: - 1
    // out: - 'yes'
    // out: - 1.0e+16
    // out: c: {}
    print(write(doc, flow: true))    // out: {b: [1, 'yes', 1.0e+16], a: 'two
    // out:
    // out:     lines', c: {}}
    print(write(Yaml.Str(String("a plain root"))))
    // out: a plain root
    // out: ...
    println("{doc}")    // out: {b: [1, 'yes', 1.0e+16], a: "two\nlines", c: {}}
}
```

`{v}` writes the value as `write` does with `flow`, with no width to fold at and with a string that has a break in it in double quotes, so that it's one line.

## How it's checked

The reading and writing were compared with PyYAML 6.0.3's pure Python `SafeLoader` and `SafeDumper` under Python 3.14:

- **Reading**: 220,000 documents: hand-written ones, documents `safe_dump` wrote from random trees with random options, and both with random edits, so that mistakes are compared too; and the 402 documents of the [YAML test suite](https://github.com/yaml/yaml-test-suite), with four edited copies of each. Of those PyYAML read to a tree Calx can hold, without a set, a lone surrogate or an integer past 64 bits, every one read to the same tree, or was refused by both, but 6: three with an alias inside the node it names, by way of a merge; a key whose value is an integer past 64 bits, which a later value for the same key replaces, so PyYAML builds it and throws it away; a `<<` in a `!!pairs` list, which PyYAML refuses and Calx merges; and a `!!str` tag on a list to merge, which PyYAML doesn't look at.
- **Writing**: 152,000 trees, those read from the documents above and 80,000 more random trees of hard strings, with control characters, breaks of each kind, quotes, indicators, runes past U+FFFF, and long lines with and without spaces, written with eight sets of options. What `write_all` wrote was byte for byte what `safe_dump_all` wrote, every time.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs. The files are what PyYAML wrote of a list of 25,000 records, like a data export; a config of 4,000 services, each with nested settings, like a big Compose or Kubernetes file; and a mix of every kind of value in a random tree. The comparisons are PyYAML 6.0.3 under Python 3.14, both pure Python and with libyaml's `CSafeLoader` and `CSafeDumper`, and Rust 1.91 with the `serde_yaml` 0.9.34 crate, into a `serde_yaml::Value` and back with `to_string`, and the `yaml-rust2` 0.10 crate, into its `Yaml` documents and back with `YamlEmitter`, built for release with `x86-64-v3`:

| Work | Calx | PyYAML | PyYAML with libyaml | serde_yaml | yaml-rust2 |
| --- | --- | --- | --- | --- | --- |
| Records, 4.3 MB, parsed | 71.9 ms | 14,782 ms | 3,523 ms | 271 ms | 204 ms |
| Config, 2.0 MB, parsed | 31.5 ms | 6,183 ms | 1,385 ms | 147 ms | 107 ms |
| Mix, 10.9 MB, parsed | 180 ms | 22,021 ms | 4,277 ms | 576 ms | 438 ms |
| Records, written | 21.8 ms | 8,318 ms | 2,554 ms | 96.1 ms | 40.4 ms |
| Config, written | 7.2 ms | 3,357 ms | 908 ms | 54.4 ms | 27.9 ms |
| Mix, written | 67.1 ms | 10,167 ms | 2,515 ms | 231 ms | 119 ms |

Both Rust crates read YAML 1.2 and write their own layout, so their trees and text differ a little from PyYAML's and Calx's.
