---
title: std.term
description: The terminal's size, standard input a key at a time, and the text that puts the terminal back however the program ends.
---

# `std.term`

The terminal, for a program that draws on it, such as a status line that stays at the bottom while output scrolls above it. It reads the terminal's size, puts standard input in raw mode to read each key as it's typed and draw it itself, and says with `at_exit` what undoes what it drew.

```calx
import std.term.{at_exit, raw, size}

fn main() {
    // The example's output is a pipe, so there's no terminal to draw on.
    if let s = unsafe { size() } {
        println("{s.columns} by {s.rows}")
    } else {
        println("no terminal")    // out: no terminal
    }
    println("{raw()}")    // out: false
    at_exit("bye\n")
    println("working")    // out: working
}
// out: bye
```

| Function | Gives |
| --- | --- |
| `unsafe size() -> Size?` | The `columns` and `rows` of the terminal standard output is, or `none` if it isn't one. On Windows it's the console's window, not its buffer |
| `raw() -> bool` | Puts standard input in raw mode until `cooked()` or the program's end. `false`, changing nothing, if standard input isn't a terminal |
| `cooked()` | Puts standard input back as it was before `raw()` |
| `at_exit(text: String)` | Says what's written to standard output as the program ends, after everything it printed. Each call replaces the last; it's at most 1024 bytes |
| `read(buf: mut List<u8>, n: usize) -> usize` | Reads up to `n` bytes of what's typed onto the end of `buf`, as `std.io.read` does, but as UTF-8 on Windows too. `n` is at least 8 |

- **Raw mode** gives each key as it's typed. The terminal doesn't echo it, edit a line, or turn Ctrl+C into a signal. Keys come as a terminal sends them: text as UTF-8, Enter as 13, Backspace as 127 or 8, Ctrl+C as 3, and keys such as the arrows as escape sequences, `ESC [ A` for up. On Windows the console sends them that way once raw mode turns on its virtual terminal input.
- **The terminal is put back however the program ends**: as `main` returns, at a panic, and at a signal from outside, SIGINT, SIGTERM, SIGHUP or SIGQUIT on POSIX, where the program hasn't set its own handler, and Ctrl+C, Ctrl+Break or the console's closing on Windows. `at_exit`'s text is written, then standard input's mode is put back, and a signal still ends the program as it would have, with the same status. Only a kill that can't be caught, such as SIGKILL, leaves the terminal as the program had it.
- **`read` on Windows** reads the console with `ReadConsoleW`, since reading its input as bytes gives it in the console's code page and loses what isn't ASCII. A character is never split between two reads there. Outside raw mode, a read that starts with Ctrl+Z is the end of the input, so Ctrl+Z then Enter ends it, as Windows' `ReadFile` makes it for Java's `System.in` and C's `read`; Ctrl+D does on Linux. From a pipe or a file, and on Linux, it's `std.io.read`.
- **`size` is `unsafe`**, as the clock is (D88): it changes as the window does, so a program that draws asks again before it draws.

`std.term` is written in Calx over the runtime: [std/term.calx](../../std/term.calx).
