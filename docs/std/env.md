---
title: std.env
description: The program's command-line arguments and environment variables, as UTF-8 Strings on every platform.
---

# `std.env`

The program's command line and its environment.

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
| `variable(name: String) -> String?` | The value of the environment variable `name`, or `none` if it isn't set or `name` can't be one: empty, or with `=` or a zero byte in it. Windows matches names without regard to case |

Run a program with arguments by putting them after `--`: `calx run tool.calx -- --threads 24` ([the calx command](../reference/command.md)).

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

`std.env` is written in Calx: [std/env.calx](../../std/env.calx).
