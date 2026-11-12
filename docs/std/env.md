---
title: std.env
description: The program's command-line arguments and environment variables, as UTF-8 Strings on every platform, and the system it runs on.
---

# `std.env`

The program's command line, its environment, and the system it runs on.

```calx
import std.env.{args}

// args: --threads
// args: 24
// args: two words

fn main() {
    let a = args()
    println("{a.len()}")    // out: 3
    for s in a {
        println("[{s}]")
    }
    // out: [--threads]
    // out: [24]
    // out: [two words]
}
```

| Function | Gives |
| --- | --- |
| `args() -> List<String>` | The arguments the program was started with, after its own name, in order |
| `program() -> String` | The program's name, for its help and messages: the last part of the path it was started as, without `.exe` or `.calx`. `calx run tools/count.calx` starts it as `tools/count.calx`, so it's `count`, and so is a program built as `count.exe` or started by a `#!` line as `./count` |
| `script() -> String?` | The `.calx` file `calx run` built the program from, as a full path, as Python's `__file__` is for the script it runs: for finding the files that go with a script, beside it, from wherever it's run. `none` for a program started another way, such as one `calx build` made, whose own path is [`std.process.current_exe()`](process.md) |
| `variable(name: String) -> String?` | The value of the environment variable `name`, or `none` if it isn't set or `name` can't be one: empty, or with `=` or a zero byte in it. Windows matches names without regard to case |
| `os() -> String` | The system the program runs on, `"windows"`, `"linux"` or `"macos"`, as Rust's `std::env::consts::OS` names them, for the few things a script does differently on each. A program is built for one system, so it's the same on every call |
| `arch() -> String` | The processor the program was built for, `"x86_64"` or `"aarch64"`, as Rust's `std::env::consts::ARCH` names them. An x64 program that Windows or macOS runs on an Arm processor, translating it as it goes, says `"x86_64"` |
| `native_arch() -> String` | The processor the system has: `arch()`, but `"aarch64"` for an x64 program that Windows on Arm or macOS's Rosetta translates. For choosing which build of a program to download, as `calx update` does. Python's `platform.machine()` says what the system says to the program, which is `AMD64` under Windows' translation |

Run a program with arguments by putting them after `--`: `calx run tool.calx -- --threads 24` ([the calx command](../reference/command.md)). [`std.cli`](cli.md) reads them into a struct, as options, flags and arguments by place, with `--help`.

A script finds what goes with it by `script()`, as a Python script does by `__file__`:

```calx
import std.env
import std.path

fn main() {
    // The folder this file is in, wherever it was started from.
    let here = path.parent_dir(env.script() ?? String()) ?? String(".")
    println("{path.is_absolute(here)}")    // out: true
}
```

`calx run` passes the path in the environment variable `CALX_SCRIPT`, which the program's runtime takes out of its environment as it starts, so a program it starts doesn't see it.

## Text

Every argument is a valid UTF-8 `String`, so the platforms differ only where an argument can't be text:

| Platform | What it passes | What `args` does |
| --- | --- | --- |
| Windows | One UTF-16 string for the whole command line | Splits it as the C runtime documents: whitespace separates arguments outside quotes, `\"` is a quote, and backslashes are literal unless they come before a quote. Converts each to UTF-8, which is exact except for an unpaired surrogate, which becomes U+FFFD |
| Linux | Bytes, already split | Keeps valid UTF-8 as it is, and turns each byte that doesn't start a valid sequence into U+FFFD |

An environment variable's value is converted in the same way, and `variable` reads the environment as it is when it's called.

```calx
import std.env.{variable}

fn main() {
    // PATH is set on every system a program runs on.
    println("{variable("PATH") != none}")    // out: true
    println("{variable("CALX_NOT_A_VARIABLE_EVER") == none} {variable("A=B") == none} {variable("") == none}")    // out: true true true
}
```

The runtime reads the command line once at startup. Each call to `args` gives new copies, which the caller owns.

```calx
import std.env.{os}

fn main() {
    let shell = if os() == "windows" { "cmd" } else { "sh" }
    println("{shell == "cmd" or shell == "sh"}")    // out: true
}
```

`std.env` is written in Calx: [std/env.calx](../../std/env.calx).
