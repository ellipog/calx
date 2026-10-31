---
title: std.io
description: Standard input and output as bytes - read, read_exact, write and flush - and standard input a line at a time with Lines.
---

# `std.io`

Standard input and output as bytes, for programs that speak a binary protocol, such as the rig's adapter, and standard input as [lines](#lines) of text. To write text, use [`print` and `println`](../reference/printing.md).

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
| `Lines()`, `lines.next() -> String?` | Standard input a line at a time ([lines](#lines)) |
| `is_terminal() -> bool` | Whether standard output is a terminal, not a file or a pipe, so a program can color it with ANSI escape codes. A Windows console reads them only once a program turns that on, which this does, and one that can't, before Windows 10, counts as no terminal |

- What `write` writes and what `print` prints share one buffer, so they come out in the order they were written.
- The buffer is written out when it fills, at `flush()`, when `read` is about to wait for input, and when the program ends. So a program that answers requests doesn't need to flush before reading the next one, but one whose peer waits for a reply without sending more must flush.
- `read` returns as soon as some input is there, which can be fewer bytes than asked for. `read_exact` loops until it has them all.
- The bytes are exact on every platform: Windows doesn't turn `\r\n` into `\n` on the way in, or `\n` into `\r\n` on the way out.
- Standard error isn't buffered: see [`eprint` and `eprintln`](../reference/printing.md).

## Lines

`Lines` reads standard input a line at a time. `next()` gives the next line, a `String`, or `none` once the input has ended:

```calx
import std.io.{Lines}

fn main() {
    var input = Lines()
    var total = 0
    while let line = input.next() {
        let n = i32.parse(line) else {
            println("not a number: {line}")
            continue
        }
        total += n
    }
    println("total {total}")
}
// stdin: 3
// stdin: four
// stdin: 5
// out: not a number: four
// out: total 8
```

Lines are split and decoded as Java's `BufferedReader.readLine` gives them over an `InputStreamReader` in UTF-8, which is how a Java program's console reads commands:

- A line ends at `
`, `
` or `
`, which isn't part of it. A last line without an end is still a line, and an input that ends with a line end has no empty line after it.
- A line that ends at `
` is given at once, without waiting to see whether a `
` comes next. One that does is skipped as part of the same end.
- Bytes that aren't UTF-8 become U+FFFD, as [`decode_lossy`](text-utf8.md) gives them, so every line reads.
- A Windows console's input is read as UTF-16 and given as UTF-8, as [`std.term.read`](term.md) gives it, since reading it as bytes would give it in the console's code page. Ctrl+Z then Enter ends the input there, as Ctrl+D does on Linux.
- Like `read`, `next()` writes out what's waiting in the output buffer before it waits for input, so a prompt printed with `print` shows.
- It reads 64 KiB at a time, and a line is the only copy made of its bytes. A line can be as long as memory allows.

`Lines` reads standard input itself, so a program that also calls `read` or `read_exact` gets bytes `Lines` hasn't taken yet only by chance: use one or the other. It was compared with Java 25 on 40 random streams of every kind of line end, bytes that aren't UTF-8 and characters cut off, which it read in random pieces, so lines ended at every place a read can: [tools/unicode/utf8_check.py](../../tools/unicode/utf8_check.py).

`std.io` is written in Calx, over five runtime functions it declares as [C functions](../reference/unsafe.md#c-functions): [std/io.calx](../../std/io.calx).
