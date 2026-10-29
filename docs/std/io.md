---
title: std.io
description: Standard input and output as bytes - read, read_exact, write and flush.
---

# `std.io`

Standard input and output as bytes, for programs that speak a binary protocol, such as the rig's adapter. For text, use [`print` and `println`](../reference/printing.md).

```calx
import std.io.{read_exact, write, flush}

fn main() {
    var buf = List<u8>()
    if read_exact(mut buf, 3) {
        write(buf)    // out: abc
        println("")
    } else {
        eprintln("only got {buf.len()} bytes")
    }
    flush()
}
// stdin: abc
```

| Function | Does |
| --- | --- |
| `read(buf: mut List<u8>, n: usize) -> usize` | Reads up to `n` bytes onto the end of `buf`, and returns how many. That's 0 only at the end of the input, or when `n` is 0. Standard input that isn't open, as a service's or a detached process's may not be, reads as empty |
| `read_exact(buf: mut List<u8>, n: usize) -> bool` | Reads exactly `n` bytes onto the end of `buf`. Returns `false` if the input ended first, keeping the bytes that came |
| `write(bytes: List<u8>)` | Writes the bytes to standard output |
| `flush()` | Writes out what's waiting in the output buffer |
| `is_terminal() -> bool` | Whether standard output is a terminal, not a file or a pipe, so a program can color it with ANSI escape codes. A Windows console reads them only once a program turns that on, which this does, and one that can't, before Windows 10, counts as no terminal |

- What `write` writes and what `print` prints share one buffer, so they come out in the order they were written.
- The buffer is written out when it fills, at `flush()`, when `read` is about to wait for input, and when the program ends. So a program that answers requests doesn't need to flush before reading the next one, but one whose peer waits for a reply without sending more must flush.
- `read` returns as soon as some input is there, which can be fewer bytes than asked for. `read_exact` loops until it has them all.
- The bytes are exact on every platform: Windows doesn't turn `\r\n` into `\n` on the way in, or `\n` into `\r\n` on the way out.
- Standard error isn't buffered: see [`eprint` and `eprintln`](../reference/printing.md).

`std.io` is written in Calx, over four runtime functions it declares as [C functions](../reference/unsafe.md#c-functions): [std/io.calx](../../std/io.calx).
