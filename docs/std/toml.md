---
title: std.toml
description: TOML 1.0 config text parsed into a Toml tree as Python's tomllib reads it, with the line and column of any mistake, written back byte for byte as tomli-w writes it, and @toml, which reads and writes a struct from its fields' types.
---

# `std.toml`

TOML 1.0, the config format of `pyproject.toml` and `Cargo.toml`: text parsed into a tree of `Toml` values as Python's `tomllib.loads` reads it, with the line and column of what's wrong when it isn't TOML, a tree written back as text, byte for byte as [tomli-w](https://github.com/hukkin/tomli-w)'s `dumps` writes it, and [`@toml`](#toml), which reads a config file straight into a struct and writes it back, from the struct's fields' types.

```calx
import std.toml.{Toml, TomlError, parse, write}

fn main() ! TomlError {
    let config = try parse(##"
        name = "lobby"

        [network]
        port = 25_565
        compression.threshold = 256

        [[backup]]
        every = 07:30:00
        "##)
    println("{config.get("name")} {config.path("network.port")} {config.path("network.compression")}")    // out: some("lobby") some(25565) some({ threshold = 256 })
    let port = (config.path("network.port") ?? Toml.Int(25565)).as_i64() ?? 25565
    println("{port + 1}")    // out: 25566
    print(write(config))
    // out: name = "lobby"
    // out: backup = [
    // out:     { every = 07:30:00 },
    // out: ]
    // out:
    // out: [network]
    // out: port = 25565
    // out:
    // out: [network.compression]
    // out: threshold = 256
    match parse(String("port = 25565\nport = 25566")) {
        ok(_) => {},
        err(e) => println("{e}"),    // out: line 2, column 1: `port` is in this table already
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `Toml` | A TOML value. See [Values](#values) |
| `Moment` | The date and time of a date-time, a date or a time. See [Dates and times](#dates-and-times) |
| `TomlError` | Why text isn't TOML, or a value isn't what `from_toml` wants: `message`, and the `line` and `column` in the text, from 1, or 0 for an error in a value. `{e}` is `line 3, column 5: ...`, or the message alone |
| `parse(text: String) -> Toml ! TomlError` | The document in `text`, a `Toml.Table`, as Python's `tomllib.loads(text)` reads it |
| `write(doc: Toml) -> String` | The table `doc` as a document, as `tomli_w.dumps(doc)` writes it. See [Writing](#writing). It panics if `doc` isn't a table, since only a table is a document |
| `{v}` | The value as TOML writes it after a key, on one line: `"text"`, `1.5`, `[1, 2]` or `{ a = 1 }` |
| `v.get(key: String) -> Toml?` | A copy of key `key`'s value, if `v` is a table that has it |
| `v.at(i: usize) -> Toml?` | A copy of element `i`, if `v` is an array that long |
| `v.path(key: String) -> Toml?` | A copy of the value a dotted key names, written as TOML writes one: `tool.black.line-length`, or `servers."eu west".port` for a part with a dot or a space. `none` if something on the way isn't there or isn't a table. Only what's found is copied |
| `v.as_str()`, `v.as_i64()`, `v.as_bool()` | The text, the integer or the bool, if `v` is one |
| `v.as_f64() -> f64?` | A float, or an integer as the nearest `f64` |
| `v.as_moment() -> Moment?` | The date and time, if `v` is a date-time, a date or a time |
| `v.len() -> usize` | An array's elements or a table's keys, and 0 for anything else |
| `v.keys() -> List<String>` | A table's keys, in order |
| `v.kind() -> String` | `a string`, `an integer`, `a float`, `a bool`, `a date-time`, `a local date-time`, `a date`, `a time`, `an array` or `a table`, for messages |
| `@toml` | On a struct, writes its `to_toml`, `from_toml`, `from_toml_at`, `to_toml_text` and `from_toml_text`. See [`@toml`](#toml) |
| `@toml_enum` | On an enum whose variants hold nothing, writes its `to_toml`, `from_toml` and `from_toml_at`, as the variants' names |

A `match` reads a value in place, without the copy that `get` makes:

```calx
import std.toml.{Toml, parse}

fn main() {
    let doc = parse(String("deps = [\"serde\", \"rand\"]\n[package]\nname = \"calx\""))!
    match doc {
        Toml.Table(t) => {
            for (k, v) in t {
                println("{k}: {v.kind()} {v.len()}")
            }
        },
        _ => {},
    }
    // out: deps: an array 2
    // out: package: a table 1
}
```

## Values

| `Toml` | Is | Python's `tomllib` gives |
| --- | --- | --- |
| `Str(String)` | A string, any of TOML's four kinds | `str` |
| `Int(i64)` | An integer, in decimal, or in hex, octal or binary after `0x`, `0o` or `0b` | `int` |
| `Float(f64)` | A float, `inf` or `nan` | `float` |
| `Bool(bool)` | `true` or `false` | `bool` |
| `OffsetDateTime(Moment)` | `1979-05-27T07:32:00-08:00`, with `Z` for UTC | `datetime` with a `tzinfo` |
| `LocalDateTime(Moment)` | `1979-05-27T07:32:00`, or with a space for the `T` | `datetime` without one |
| `LocalDate(Moment)` | `1979-05-27` | `date` |
| `LocalTime(Moment)` | `07:32:00`, with the seconds | `time` |
| `Array(List<Toml>)` | An array, whose values can be of any kinds | `list` |
| `Table(LinkedMap<String, Toml>)` | A table, its keys in the order the text has them | `dict` |

A table holds the tables its headers and dotted keys make, and an array of tables, `[[name]]`, is an `Array` of `Table`s, as in Python. `==` compares trees, so a document read back from what `write` wrote is `==` to the one written.

## Dates and times

| `Moment` field | Is |
| --- | --- |
| `year`, `month`, `day` | The date: 1 to 12 and 1 to 31 |
| `hour`, `minute`, `second` | The time of day: 0 to 23, 0 to 59 and 0 to 59 |
| `nano` | The fraction of a second, in nanoseconds. Digits past the ninth are dropped, as TOML says |
| `offset` | The seconds an offset date-time is ahead of UTC, as [std.time](time.md) counts them: -28,800 for `-08:00`, and 0 for `Z` |

The parts a value doesn't have are 0: a date's time, and a time's date. `m.date_time()` is std.time's `DateTime`, to the millisecond, and `m.timestamp()` is the moment as a `Timestamp`, at its offset.

```calx
import std.toml.{Moment, Toml, parse}

fn main() {
    let doc = parse(String("released = 2026-11-08T14:05:09.250+01:00"))!
    let m = (doc.get("released") ?? Toml.Bool(false)).as_moment() ?? Moment {}
    println("{m.hour} {m.nano} {m.offset} {m.date_time().iso()} {m.timestamp().iso(0)}")    // out: 14 250000000 3600 2026-11-08T14:05:09.250 2026-11-08T13:05:09.250Z
}
```

## Reading

`parse` reads TOML 1.0 as its spec and `tomllib` do:

- **Keys** are bare, `max-players`, or quoted, `"eu west"` and `'eu west'`, and dotted, `network.port`, with white space around the dots if wanted. A dotted key makes the tables on the way.
- **A header**, `[a.b]`, starts the table its keys go in, and makes the tables on the way. A table can't have two headers, nor one after dotted keys made it, and dotted keys can't add to a table a header made, or that another section's dotted keys made.
- **An array of tables**, `[[a]]`, gets a new table at each header. A header in it, `[a.b]`, goes in its last table.
- **Strings** are basic, `"..."`, with escapes, `\t`, `\"`, `\u00E9` and `\U0001F600`; literal, `'...'`, as they are; and either kind multi-line, between `"""` or `'''`, without the newline right after the opening quotes. In a multi-line basic string, a `\` at the end of a line leaves out the newline and the white space after it.
- **Numbers** have single `_`s between digits if wanted. An integer has no leading zeros, and must fit 64 bits.
- **Inline tables**, `{ a = 1, b.c = 2 }`, are on one line, with no comma after the last key. Arrays can span lines, with comments, and a comma after the last value.
- **Newlines** are `\n` or `\r\n`. A `\r\n` in a multi-line string is read as `\n`, as `tomllib` reads it. Any other control character, but the tab, is an error outside an escape, in a string or a comment.

An error gives the line and column of the mistake, and says what's wrong in words, where `tomllib` gives short ones, such as `Cannot overwrite a value`:

```calx
import std.toml.{parse}

fn main() {
    for text in ["[t]\nx = 1\n[t]", "a = \{ b = 1, \}", "a = 0755", "when = 2026-02-29", "a = \"\\x41\""] {
        match parse(String(text)) {
            ok(_) => {},
            err(e) => println("{e}"),
        }
    }
    // out: line 3, column 1: `[t]` is a second header for the same table
    // out: line 1, column 14: `}` where a key should be
    // out: line 1, column 5: a number that starts with `0` and goes on, which TOML doesn't allow
    // out: line 1, column 8: a date that isn't one; TOML writes them `1979-05-27`
    // out: line 1, column 6: `x` after `\` in a string, which isn't an escape
}
```

It differs from `tomllib` only where TOML or RFC 3339, its dates' standard, say otherwise, and in how deep values nest:

- **An integer past 64 bits is an error**, as TOML says. `tomllib` reads it as a Python `int`.
- **Fractions of a second are kept to the nanosecond.** `tomllib` drops the digits past the microsecond, as Python's `datetime` has no more.
- **Year 0 is a date**, as RFC 3339 has it. Python's `date` starts at year 1, so `tomllib` gives an error.
- **Arrays and inline tables nest 512 deep at most.** `tomllib` runs out of Python's stack at 498, with a `RecursionError`.
- **A byte order mark at the start is an error**, as in `tomllib`, which reads text and not bytes. Save TOML as UTF-8 without one.

## Writing

`write` writes a document as tomli-w does, so a file Python wrote and Calx writes again comes out the same:

- A table's values come first, each after its key, then its tables, each as a section under a header of its own with a blank line before it. A table that holds only tables has no header itself.
- An array of tables is inline, `[ { x = 1 }, ... ]`, if each of its tables fits on a line of 100 runes, written inline as an element; otherwise each table is a section under `[[name]]`.
- An array that isn't empty has each element on a line of its own, indented by 4 spaces a level, with a comma after each. An inline table is `{ a = 1, b = 2 }`, or `{}`.
- A string is a basic string: `"` and `\` escaped, and each control character but the tab, as `\n` and the like or `\u001b`. A key is bare if it can be, and otherwise a basic string too.
- A float is written as Python's `repr` writes it: `1.0`, `0.0001`, `1e+16`, `1.5e-05`, `inf` and `nan`.
- A date-time is written as Python's `str` writes one, as tomli-w does: `1979-05-27 07:32:00-08:00`, with a space for the `T` and `+00:00` for `Z`, and the fraction of a second in 6 digits, or 9 for one finer than a microsecond.

```calx
import std.toml.{Toml, write}

fn main() {
    var server = LinkedMap<String, Toml>()
    server.insert(String("motd"), Toml.Str("Hi, \"you\"\n"))
    server.insert(String("ratio"), Toml.Float(1e16))
    server.insert(String("ops"), Toml.Array([Toml.Str("ann"), Toml.Str("bo")]))
    var doc = LinkedMap<String, Toml>()
    doc.insert(String("server"), Toml.Table(server))
    print(write(Toml.Table(doc)))
    // out: [server]
    // out: motd = "Hi, \"you\"\n"
    // out: ratio = 1e+16
    // out: ops = [
    // out:     "ann",
    // out:     "bo",
    // out: ]
}
```

## `@toml`

`@toml` on a struct writes five functions for it, from its fields' types, as `@json` does for JSON ([std.json](json.md#json)):

| Function | Does |
| --- | --- |
| `s.to_toml() -> Toml` | The struct as a table with a key for each field, in the fields' order |
| `s.to_toml_text() -> String` | That, as a document, as `write` writes it |
| `T.from_toml(value: Toml) -> T ! TomlError` | The struct a table describes. Keys it has no field for are passed over |
| `T.from_toml_at(value: Toml, path: String) -> T ! TomlError` | The same for a value deep in a document, whose errors start with `path` |
| `T.from_toml_text(text: String) -> T ! TomlError` | The struct a TOML document describes |

`@toml_enum` on an enum whose variants hold nothing writes `to_toml`, `from_toml` and `from_toml_at` for it, as the names of its variants, so a struct with `@toml` can have a field of its type: `mode = "Survival"`. A string that names no variant is an error that lists them, `` `Config.mode` should be one of `Survival`, `Creative`, not `Adventure` ``.

```calx
import std.toml.{toml, TomlError}

@toml
struct Backup {
    every: String
    keep: u32 = 7
}

@toml
struct Config {
    name: String
    @toml_key("max-players")
    max_players: u32
    motd: String?
    port: u16 = 25565
    backup: List<Backup>
}

fn main() ! TomlError {
    let c = try Config.from_toml_text(##"
        name = "lobby"
        max-players = 50

        [[backup]]
        every = "6h"

        [[backup]]
        every = "1d"
        keep = 30
        "##)
    println("{c.name} {c.max_players} {c.motd} {c.port} {c.backup[0].keep} {c.backup[1].keep}")    // out: lobby 50 none 25565 7 30
    print(c.to_toml_text())
    // out: name = "lobby"
    // out: max-players = 50
    // out: port = 25565
    // out: backup = [
    // out:     { every = "6h", keep = 7 },
    // out:     { every = "1d", keep = 30 },
    // out: ]
    match Config.from_toml_text(String("name = 'x'\nmax-players = 300000000000\nbackup = []")) {
        ok(_) => {},
        err(e) => println("{e}"),    // out: `Config.max-players` should be from 0 to 4294967295, not 300000000000
    }
}
```

The fields can be of these types:

| Field | In TOML |
| --- | --- |
| `bool` | `true` or `false` |
| `i8` to `i64`, `u8` to `u64`, `usize` | An integer, checked against the type's range. `1.0` isn't one. Writing a `u64` or `usize` past `i64`'s range panics, since TOML's integers are 64-bit and signed |
| `f64`, `f32` | A float, or an integer |
| `String` | A string |
| `Toml` | Anything, as it is: for a date or a time, or a value of more than one kind |
| `T?` | What `T` is, or no key for `none`, since TOML has no null |
| `List<T>` | An array of what `T` is. A list of structs is an array of tables |
| `Map<String, T>`, `LinkedMap<String, T>`, `SortedMap<String, T>` | A table whose values are what `T` is, written in the map's order |
| A struct with `@toml`, or an enum with `@toml_enum`, of its own | A table, or the name of a variant, from its own module too, once it's imported |

- **A key that isn't there** is `none` for an optional field, and the field's default if it has one, as `port` has above. For any other field it's an error, `` `Config.name` is missing ``.
- **`@toml_key("...")` on a field** names its key in TOML, for a key that isn't a Calx name, such as `max-players`. A field is written under its own name otherwise.
- **An error names the path to what's wrong**: the struct's name, then a key after a `.` and an array's index in `[]`, through the structs inside it: `` `Config.ops[0]` should be a string, not an integer ``, `` `Config.network.port` should be from 0 to 65535, not 70000 ``. An error in the text, before there's a table to read, has its line and column.
- `@toml` refuses a generic struct, a map whose keys aren't `String`, and a field of any other type, such as a tuple, with why, at the `@toml`. A struct field whose type has no `@toml` of its own is an error in the code `@toml` writes, that the type has no `to_toml`; give that type `@toml` too.
- `calx expand` prints the code `@toml` writes, which is ordinary Calx.

## How it's built

[std/toml.calx](../../std/toml.calx) is written in Calx from TOML 1.0's spec. A header can add to a table that's already there, as can a dotted key, so `parse` builds tables by number, each with its values and the numbers of the tables it holds. At the end it makes them into `Toml` values from the last back, since each table's own tables come after it. A table remembers whether a header named it, or which section's dotted keys made it, which is how it knows what TOML lets add to it. An inline table is made into a value at its `}`.

The text is read a byte at a time, and the runs of a string between its escapes are copied whole.

## Checked against Python

On the [toml-test](https://github.com/toml-lang/toml-test) suite's 717 TOML 1.0 files, `parse` gives what `tomllib` gives for each: the same values for all 208 valid ones, and an error for all 509 invalid ones, as the suite says it should.

It also matches `tomllib` on 220,000 random documents, 124,810 of them valid and the rest broken by a few random edits each, apart from 137 with an integer past 64 bits. They had every kind of key and value, every way to write each, headers, dotted keys, arrays of tables, inline tables, comments, and `\r\n`. `write` matches tomli-w 1.2.0 byte for byte on 120,000 random trees, with random keys, strings with control characters, floats of random bits, every kind of date and time, and arrays of tables both inline and not.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs. The files are a lock file like `Cargo.lock`, an array of 24,000 tables; a config of 16,000 sections, each with numbers, strings, a date-time, dotted keys, an array and an inline table; and a mix of every kind of value, which tomli-w wrote from a random tree. The comparisons are Python 3.14's `tomllib` and tomli-w 1.2.0, and Rust 1.91 with the `toml` crate 0.9.12, built for release with `x86-64-v3`, into a `toml::Table` and back with `toml::to_string`:

| Work | Calx | Python | Rust |
| --- | --- | --- | --- |
| Lock file, 6.6 MB, parsed | 19.5 ms | 651 ms | 107 ms |
| Config, 4.2 MB, parsed | 50.5 ms | 1,552 ms | 208 ms |
| Mix, 6.0 MB, parsed | 58.9 ms | 1,475 ms | 243 ms |
| Lock file, written | 7.8 ms | 590 ms | 49.9 ms |
| Config, written | 29.0 ms | 527 ms | 85.4 ms |
| Mix, written | 44.5 ms | 629 ms | 152 ms |

Rust's `toml` writes its own layout, so its text differs a little from tomli-w's and Calx's.
