---
title: std.io
description: Standard input and output as bytes - read, read_exact, write and flush - standard input a line at a time with Lines, and Reader and Writer, what files, connections, compressors and archives read from and write to.
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
| `reader_gone() -> bool` | Whether standard output's reader has gone, so what's written there is dropped ([below](#when-the-reader-has-gone)) |
| `is_terminal() -> bool` | Whether standard output is a terminal, not a file or a pipe, so a program can color it with ANSI escape codes. A Windows console reads them only once a program turns that on, which this does, and one that can't, before Windows 10, counts as no terminal |

- What `write` writes and what `print` prints share one buffer, so they come out in the order they were written.
- The buffer is written out when it fills, at `flush()`, when `read` is about to wait for input, and when the program ends. So a program that answers requests doesn't need to flush before reading the next one, but one whose peer waits for a reply without sending more must flush.
- `read` returns as soon as some input is there, which can be fewer bytes than asked for. `read_exact` loops until it has them all.
- The bytes are exact on every platform: Windows doesn't turn `\r\n` into `\n` on the way in, or `\n` into `\r\n` on the way out.
- Standard error isn't buffered: see [`eprint` and `eprintln`](../reference/printing.md).

## When the reader has gone

A program's standard output can lose its reader while the program runs: `head` reads the lines it wants and ends, or a supervisor that read a program's output stops reading. Writing there then fails, and never ends the program. That's the same on every system: a Calx program ignores SIGPIPE from the start, as Python and Rust do, where on Linux and macOS the write would otherwise end it with that signal, status 141 in a shell, as it did before (CLX-433) unless the program had made a [`std.http`](http.md) request, which ignored it too.

- **`write`, `flush`, `print` and `println` drop what they can't write**, quietly, and so does everything after, without trying. The program goes on, and ends with its own status, so a server whose log went to a pipe that closed keeps serving.
- **`reader_gone()` says so**, from the first write that failed. Standard output is buffered, so that's when the buffer was written out, at `flush()` or once 64 KiB is waiting, not at the `print` that filled it.
- **`Stdout`'s `write` and `flush` fail**, with `standard output's reader has gone (broken pipe)`, as Python's `print` raises `BrokenPipeError`. Code that copies to any `Writer` stops there, as it would at a file it can't write.
- **The programs it starts get the default back**, so `yes | head` run with [`std.process`](process.md) ends as it does from a shell.

A program that prints without end stops when its reader goes, as `yes` does:

```
import std.io.{flush, reader_gone}

fn main() {
    var n: u64 = 0
    while not reader_gone() {
        println("{n}")
        n += 1
        if n % 1000 == 0 {
            flush()
        }
    }
}
```

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

`Lines` reads standard input itself, so a program that also calls `read` or `read_exact` gets bytes `Lines` hasn't taken yet only by chance: use one or the other. It was compared with Java 25 on 40 random streams of every kind of line end, bytes that aren't UTF-8 and characters cut off, which it read in random pieces, so lines ended at every place a read can: [tools/unicode/utf8_check.calx](../../tools/unicode/utf8_check.calx).

## Reader and Writer

`Reader` and `Writer` are what streams of bytes read from and write to, as Python's file objects opened `"rb"` and `"wb"` are, so code that reads or writes a stream takes any of them: [`std.tar`](tar.md#a-piece-at-a-time)'s `StreamReader` and `StreamWriter`, and [`std.zstd`](zstd.md#a-piece-at-a-time)'s `Reader` and `Writer`, which are a `Reader` and a `Writer` themselves, so they go between.

```calx
import std.io.{ByteReader, Reader, Writer}

// Copies all `from` gives to `to`, as Python's `shutil.copyfileobj` does.
fn copy_all<R: Reader, W: Writer>(from: mut R, to: mut W) -> usize ! String {
    var buf = List<u8>()
    var total: usize = 0
    loop {
        buf.clear()
        let got = try from.read(mut buf, 65536)
        if got == 0 {
            break
        }
        total += got
        try to.write(buf)
    }
    try to.flush()
    return total
}

fn main() {
    var from = ByteReader.new("some bytes".to_bytes())
    var to = List<u8>()
    println("{copy_all(mut from, mut to)!} {String.from_utf8(to)!}")    // out: 10 some bytes
}
```

| Item | Does |
| --- | --- |
| `interface Reader`, `r.read(buf: mut List<u8>, n: usize) -> usize ! String` | Reads up to `n` bytes onto the end of `buf`, and returns how many, waiting until there's at least one. That's 0 only at the end, or when `n` is 0 |
| `interface Writer`, `w.write(bytes: List<u8>) ! String` | Writes all the bytes |
| `w.flush() ! String` | Writes out what it holds back, so the other end gets everything written so far. By default it does nothing |
| `ByteReader.new(bytes: own List<u8>) -> ByteReader`, `b.left() -> usize` | Bytes in memory read from the start, as `io.BytesIO(data)`, and how many are left |
| `Stdin {}`, `Stdout {}` | Standard input as a `Reader`, which reads as `read` does, and standard output as a `Writer`, which writes and flushes as `write` and `flush` do, but fails once the reader has gone ([above](#when-the-reader-has-gone)) |

They fail with what went wrong, in words, a `String`, so one `Reader` or `Writer` can wrap another of any kind. These have them:

| Type | Reader | Writer |
| --- | --- | --- |
| [`std.fs`](fs.md#files-kept-open)'s `File` | From where the last read stopped | From where the last write stopped; `create` opens one empty |
| `List<u8>` | | Grows by what's written |
| `ByteReader` | Its bytes | |
| `Stdin`, `Stdout` | Standard input | Standard output |
| [`std.net`](net.md#a-connection-as-a-stream)'s `Stream` | A connection's bytes, waiting for them | Queued on the connection, waiting once 4 MiB is |
| `std.zstd`'s `Reader` and `Writer` | What it decompresses from another `Reader` | Compressed onto another `Writer` |
| `std.tar`'s `StreamReader` | The entry it's at | |

`std.io` is written in Calx, over seven runtime functions it declares as [C functions](../reference/unsafe.md#c-functions): [std/io.calx](../../std/io.calx).
