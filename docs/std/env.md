---
title: std.env
description: The program's command-line arguments, as UTF-8 Strings on every platform.
---

# `std.env`

The program's command line.

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

Run a program with arguments by putting them after `--`: `calx run tool.calx -- --threads 24` ([the calx command](../reference/command.md)).

## Text

Every argument is a valid UTF-8 `String`, so the platforms differ only where an argument can't be text:

| Platform | What it passes | What `args` does |
| --- | --- | --- |
| Windows | One UTF-16 string for the whole command line | Splits it as the C runtime documents: whitespace separates arguments outside quotes, `\"` is a quote, and backslashes are literal unless they come before a quote. Converts each to UTF-8, which is exact except for an unpaired surrogate, which becomes U+FFFD |
| Linux | Bytes, already split | Keeps valid UTF-8 as it is, and turns each byte that doesn't start a valid sequence into U+FFFD |

The runtime reads the command line once at startup. Each call to `args` gives new copies, which the caller owns.

`std.env` is written in Calx: [std/env.calx](../../std/env.calx).
