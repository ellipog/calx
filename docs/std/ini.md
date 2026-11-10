---
title: std.ini
description: INI files read, given and written exactly as Python's configparser does it with its defaults, with sections, comments, values on several lines, [DEFAULT], keys that aren't case-sensitive and %(name)s, and the line of any mistake.
---

# `std.ini`

INI files, the config format of `setup.cfg`, `tox.ini` and many tools on Windows, as Python's `configparser.ConfigParser()` reads, gives and writes them with its defaults: sections in `[...]`, keys with `=` or `:` and their values, comments, values on several lines, a `[DEFAULT]` section whose keys every section has, keys that aren't case-sensitive, and `%(name)s` in a value for another key's value.

```calx
import std.ini.{parse}

fn main() ! Error {
    var config = try parse(##"
        [DEFAULT]
        home = /srv

        [server]
        Port = 8080
        root = %(home)s/www
        debug = yes
        motd = Welcome,
          and enjoy your stay.
        "##)
    let port = try config.get_int("server", "port") ?? 80
    let workers = try config.get_int("server", "workers") ?? 4
    let debug = try config.get_bool("server", "debug") ?? false
    println("{port} {workers} {debug}")    // out: 8080 4 true
    println("{try config.get("server", "root") ?? String()}")    // out: /srv/www
    println("{(try config.get("server", "motd") ?? String()).replace("\n", " / ")}")    // out: Welcome, / and enjoy your stay.
    try config.set("server", "port", "8081")
    try config.add_section("logs")
    try config.set("logs", "level", "info")
    print(try config.write())
    // out: [DEFAULT]
    // out: home = /srv
    // out:
    // out: [server]
    // out: port = 8081
    // out: root = %(home)s/www
    // out: debug = yes
    // out: motd = Welcome,
    // out: 	and enjoy your stay.
    // out:
    // out: [logs]
    // out: level = info
    // out:
    match parse("[server]\nport = 8080\nPORT = 8081\n") {
        ok(_) => {},
        err(e) => println("{e}"),    // out: line 3: a second `port` in [server]
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `parse(text: String) -> Ini ! IniError` | `text` read as an INI file. Python's `ConfigParser().read_string(text)` |
| `Ini.new() -> Ini` | A file with no sections. `ConfigParser()` |
| `ini.read(text: String) ! IniError` | More text read into the file, a section that's there getting its keys and a key that's there its new value. A second `read_string` |
| `ini.sections() -> List<String>` | The sections' names, in order, without `DEFAULT` |
| `ini.has_section(name) -> bool`, `ini.has_key(section, key) -> bool` | Whether there's such a section, and whether the section or `[DEFAULT]` has the key. `has_section`, `has_option` |
| `ini.keys(section) -> List<String>?` | The section's keys and then `[DEFAULT]`'s. `options` |
| `ini.get(section, key) -> String? ! IniError` | The key's value, from the section or else `[DEFAULT]`, with its `%(name)s` replaced. `get` |
| `ini.raw(section, key) -> String?` | The value as it was read. `get(..., raw=True)` |
| `ini.get_int`, `ini.get_float`, `ini.get_bool` | The value as an `i64`, an `f64` or a `bool`. `getint`, `getfloat`, `getboolean` |
| `ini.items(section) -> LinkedMap<String, String>? ! IniError` | Each key `[DEFAULT]` and the section have, with its value as `get` gives it, `[DEFAULT]`'s first. `items(section)` |
| `ini.set(section, key, value) ! IniError` | A key set, in `[DEFAULT]` for a section of `DEFAULT` or `""`. `set` |
| `ini.add_section(name) ! IniError`, `ini.remove_section(name) -> bool`, `ini.remove_key(section, key) -> bool ! IniError` | `add_section`, `remove_section`, `remove_option` |
| `ini.write(spaces: bool = true) -> String ! IniError` | The file as text, `key = value`, or `key=value` without `spaces`. `write(f, space_around_delimiters=...)` |
| `IniError` | What's wrong, as its `message`, and the `line` it's on, from 1, or 0 for a mistake that isn't on a line |

Where Python raises `NoSectionError` or `NoOptionError` from `get`, `get` gives `none`, so `?? default` is Python's `fallback=default`. A mistake in a value, such as a `%(name)s` whose `name` isn't there, is an `IniError` when the value is asked for, as Python's `get` raises it then, and not when the file is read.

## How a file is read

- **A section** starts with a line in `[` and `]`. Its name is what's between the `[` and the line's last `]`, spaces included, and anything after the `]` doesn't count. Names are case-sensitive, so `[DEFAULT]` is the defaults and `[default]` is a section like any other. A section twice in one text is a mistake.
- **A key** is a line with `=` or `:`, the first of them ending the key. The key and value don't keep the white space around them, and keys are lowercased, so `Port` and `PORT` are one key, which twice in a section is a mistake.
- **Comments** are lines that start with `#` or `;`, after any white space. A `#` or `;` later in a line is part of the value.
- **A value goes on** across the lines after it that are indented more than its key, joined with newlines. Blank lines in it stay, but not at its end, and a comment line doesn't end it.
- **White space** is Python's: spaces, tabs, other ASCII white space and the Unicode spaces, such as `U+3000`. Indents count runes, as Python counts characters.
- **`[DEFAULT]`**'s keys are every section's, unless a section has the key itself. It can come more than once.
- **Mistakes** are a line before the first section, a section or key twice, and a line that's no section, key or comment. Python reads on past the last kind and raises at the end, unless a worse one comes first, and std.ini gives the same mistake, at the first such line.

## Values

`get` replaces each `%(name)s` in a value with `name`'s value, from the same section or `[DEFAULT]`, whose own `%(name)s` are replaced too, and each `%%` with `%`, as Python's `BasicInterpolation` does. Any other `%` is a mistake, as are values that refer to each other, more than 10 deep, and `set` won't take a value with one. `raw` gives the value as it was read.

`get_int` reads a value as Python's `int()` does, with a sign, `_` between digits, and digits from any script, and `get_float` as `float()` does, with `inf`, `infinity` and `nan` in any case. `get_bool` takes `1`, `yes`, `true` and `on` as `true`, and `0`, `no`, `false` and `off` as `false`, in any case. Anything else is an `IniError`.

## Writing

`write` writes `[DEFAULT]` first, if it has keys, then each section in order, with a blank line after each. Each key is `key = value` with the raw value, and a value's lines after its first start with a tab, which reads back as the same value. Comments aren't kept. A key with `=` or `:` in it, or one that starts as a `[section]` does, which `set` can make, is a mistake to write, as in Python.

## Where it differs

- A byte order mark at the start of the text is left out, as Python's `encoding="utf-8-sig"` leaves it out. Python's `read_string` takes it as part of the first line.
- `get_int` gives a mistake for a number too big for an `i64`, where Python's `int` has no limit.
- `parse` ends lines at `\n`, as `read_string` does. A `\r\n` ends one too, since the `\r` is white space, but a lone `\r`, as old Mac files have, doesn't, where Python's `read(path)` opens the file in a way that would.

## Checked against Python

std.ini gives the same as Python 3.14's `configparser` on 105,000 random files. The checks covered:

- **Reading:** the mistake and its line, or each section and its raw values.
- **Lookups:** `get`, `get_int`, `get_float` and `get_bool` for every key, `keys`, and `items`.
- **Writing:** `write` with and without spaces.
- **Changes:** random `set`, `add_section`, `remove_key` and `remove_section` calls, and `write` after them.

The files were built from the parts that trip readers up:

- odd and repeated headers, keys that differ only in case, and empty keys;
- Unicode keys and indents, continuation lines, and comments inside values;
- every kind of `%` reference, numbers in Python's forms and others, and lines ending in `\r\n`, `\r`, `\x0b`, `\x85` or ` `.

## Speed

These numbers are from our benchmark:

- **Machine:** an i9-13900K otherwise quiet, each program on its performance cores, the best of 20 runs taken in turns (12 for Python).
- **File:** 13.0 MB of INI text, 20,000 sections of 10 to 40 keys each, with comments and blank lines.
- **Work:** each value is read with `get`, which gets its key by name, for each key `keys` gives.
- **Comparisons:** Python 3.14's `configparser`, and Rust 1.91 with the `rust-ini` crate 0.21.3, built for release with `x86-64-v3`.

| Work | Calx | Python | Rust |
| --- | --- | --- | --- |
| Read | 65.5 ms | 1,657 ms | 213 ms |
| Each value got | 39.5 ms | 740 ms | 38.2 ms |
| Written | 22.8 ms | 420 ms | 61.0 ms |

`rust-ini` reads its own kind of INI by default: a value goes on across lines only in quotes, not when the lines are indented, a `\` in a value starts an escape, keys are case-sensitive, and `%(name)s` stays as it is. The file has none of these, and all three read the same values from it. Rust's `get` hands back a view of the value, where Calx's and Python's give a copy, and Calx's also builds the list of each section's keys, as Python's `options` does.
