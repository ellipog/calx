---
title: std.cli
description: "@cli on a struct reads a program's command-line arguments into it: options, flags, arguments by place and lists, with --help written from its docs, as clap's derive and argparse do."
---

# `std.cli`

`@cli` on a struct reads the program's command-line arguments into it, as Rust's clap derive and Python's argparse do. Each field is an option named after it, its type says what it takes, and `--help` is written from the struct's and the fields' `///` docs.

```calx
import std.cli.{cli, choices}

@choices
enum Format {
    Plain
    Json
}

/// Counts the lines in files.
@cli(name: "count")
struct Args {
    /// The files to read.
    @positional
    files: List<String>
    /// Count words too.
    @short("w")
    words: bool
    /// Stop after this many lines.
    @short("n")
    max_count: u32 = 100
    /// How to print the counts.
    format: Format = Format.Plain
    /// Where to write them, rather than standard output.
    out: String?
}

fn main() {
    let a = Args.parse_from(["notes.txt", "-w", "--max-count", "5", "--format=json"])!
    println("{a.files} {a.words} {a.max_count} {a.format == Format.Json} {a.out}")    // out: [notes.txt] true 5 true none
    print(Args.help())
    // out: Counts the lines in files.
    // out:
    // out: Usage: count [options] [files]...
    // out:
    // out: Arguments:
    // out:   [files]...                   The files to read.
    // out:
    // out: Options:
    // out:   -w, --words                  Count words too.
    // out:   -n, --max-count <MAX_COUNT>  Stop after this many lines. [default: 100]
    // out:       --format <FORMAT>        How to print the counts. [one of: plain, json] [default: plain]
    // out:       --out <OUT>              Where to write them, rather than standard output.
    // out:   -h, --help                   Print this help.
}
```

A real program calls `Args.parse()`, which reads its own arguments, prints the help for `--help` and ends with status 0, and for a mistake prints what's wrong and ends with status 2:

```
$ calx count.calx --max-count lots
error: `--max-count` takes an integer, not `lots`

Usage: count [options] [files]...

For more, try `--help`.
```

## Items

| Item | Does |
| --- | --- |
| `@cli`, `@cli(name: "...")` | On a struct: `T.parse()`, `T.parse_from(args)` and `T.help()`, from its fields. `name` names the program in the help, which is [`std.env.program()`](env.md) otherwise |
| `@short("n")` on a field | Adds `-n` for it |
| `@positional` on a field | Makes it an argument by place, rather than an option |
| `@choices` | On an enum whose variants hold no data: `T.from_choice(text)` and `T.choices()`, so a field can be one of its variants |
| `T.parse() -> T` | The program's arguments, read into a `T`. `--help` and mistakes end the program, as above |
| `T.parse_from(args: List<String>) -> T ! CliError` | `args` read the same way, with `--help` and mistakes given back as a `CliError`: for tests, and arguments from elsewhere |
| `T.help() -> String` | The help, as `--help` prints it |
| `CliError` | `message`, what's wrong or the help; `help`, whether it's the help; and `usage`, the usage line |

## Fields

| Field | On the command line |
| --- | --- |
| `name: String`, a number, or an enum with `@choices` | `--name value` or `--name=value`, once. Needed unless it has a default |
| `name: T?` | The same, but it can be left out, giving `none` |
| `name: List<T>` | `--name value` any number of times, in order |
| `name: bool` | A flag, `--name`, `true` when it's given |
| `@positional name: T` | The next argument by place. `T?` or a default makes it one that can be left out |
| `@positional name: List<T>` | The arguments by place that are left |

- An option is named after its field, with `-` for `_`: `max_count` is `--max-count`.
- Numbers are read with [`T.parse`](../reference/integers.md#parsing): any integer type, `f32` and `f64`. A value that isn't one, or is out of range, is a mistake that says what it takes.
- An enum's choices are its variants in kebab-case: `JsonLines` is `json-lines`.
- A field's `///` doc is its line of the help, with its default, and an enum's choices, after it. The struct's doc is the help's first paragraph.
- Short flags go together, `-wv`, and a short option's value can follow it, `-n5` or `-n 5`.
- `--` ends the options: every argument after it is by place, even one starting with `-`. `-` alone is by place too, as programs read it for standard input.
- An option given twice is a mistake, unless it's a list. So is an unknown one, which says which option it's close to, if any: "`--max-cont` isn't an option; did you mean `--max-count`?".

## What it refuses

`@cli` refuses a struct it can't read, with why, at the `@cli`:

- A generic struct, and a field of any other type, such as a tuple or a map.
- A `bool` that's optional, a list or by place, or that starts `true`: a flag is `false` until it's given, so name it for what giving it does, such as `--no-color`.
- `@short("h")`, and a field named `help`: `-h` and `--help` are the help's. Two fields with one letter.
- An argument by place after a list by place, and one that's needed after one that isn't.
- An enum field whose enum has no `@choices` is an error in the code `@cli` writes, that the enum has no `from_choice`. Give it `@choices`.

## How it works

`@cli` is an [attribute](../reference/attributes.md), written in Calx with [`std.meta`](meta.md). The code it writes builds a `CliParser` with a line for each field, runs it over the arguments, then reads each field from what it found, with the field's own type's `parse` or its enum's `from_choice`. `calx expand` shows that code, which is ordinary Calx. Reading the arguments happens once, as the program starts, so it's written to be clear rather than fast.
