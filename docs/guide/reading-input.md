---
title: Reading the commands
description: The guide's second page - reading standard input a line at a time with std.io.Lines, optionals and while let, and splitting a line into words with a function of your own.
---

# Reading the commands

The stash reads its commands from standard input, one a line. This page reads them and splits each into words. The [next](commands.md) works out what they mean.

## A line at a time

```calx
import std.io.{Lines}

fn main() {
    var input = Lines()
    var number = 0
    while let line = input.next() {
        number += 1
        println("{number}: {line}")
    }
}
// stdin: add stone 64
// stdin: show
// out: 1: add stone 64
// out: 2: show
```

- `import std.io.{Lines}` brings in `Lines` from [`std.io`](../std/io.md), one of the standard library's modules. A program imports what it uses by name, and the checker says so if an import isn't used.
- `Lines()` makes a value of the struct `Lines` with its fields at their defaults. It's a `var`, since reading a line changes it: it keeps what it read past the line's end for the next one.
- `input.next()` gives a `String?`, an [optional](../reference/optionals.md): a `String` while there are lines, or `none` once the input has ended. Calx has no null, so a value that might not be there says so in its type, and the code must check before it can use the value.
- `while let line = input.next()` is how that check is written for a loop: it runs the body with `line` bound to each line, and stops at `none`.

The `// stdin:` comments are the input the test suite gives the example. To try it yourself, run it and type some lines. Ctrl+D ends the input on Linux, and Ctrl+Z then Enter does on Windows. Or give it a file of commands:

```sh
calx run stash.calx < commands.txt
```

A line comes without the `\n` or `\r\n` that ended it, so it reads the same on Windows and Linux, and bytes that aren't UTF-8 come as `�` rather than as an error. `Lines` reads lines exactly as the Java `BufferedReader` that a Minecraft server's console uses does.

## Words

A command is words separated by spaces, such as `add stone 64`. A function of our own splits a line into them:

```calx
// The words in `line`, which spaces separate.
fn words(line: String) -> List<String> {
    var out = List<String>()
    var start: usize = 0
    for i in 0..line.len() {
        if line.byte_at(i) == 32 {    // a space
            if i > start {
                out.push(line[start..i])
            }
            start = i + 1
        }
    }
    if start < line.len() {
        out.push(line[start..])
    }
    return out
}

fn main() {
    println("{words("add stone 64")}")    // out: [add, stone, 64]
    println("{words("  take   dirt 3 ")}")    // out: [take, dirt, 3]
    println("{words("")}")    // out: []
}
```

- `List<String>` is a list of strings, which grows as `push` adds to it ([lists](../reference/lists.md)). `{out}` in a string shows a list as `[a, b]`.
- `line.len()` is the length in bytes, a `usize`, the type for sizes and indexes. `start` is declared as a `usize` too, because the two are compared, and Calx never converts between number types by itself: an `i32` and a `usize` can't be compared until one is converted ([conversions](../reference/conversions.md)).
- `line.byte_at(i)` is the byte at `i`, a `u8`, and 32 is a space. Calx text is UTF-8, where a space is always one byte, and a byte of a longer character is never 32, so splitting at that byte splits at the spaces.
- `line[start..i]` is the text from byte `start` up to `i`, and `line[start..]` from `start` to the end. A part of a `String` is a `String`, which is copied only where it's kept, here in the list ([slicing](../reference/strings.md#slicing)).
- `words` takes `line` as it is: a parameter is only looked at, unless the function says otherwise, so the caller still has it after the call. That's how `main` can pass a literal, which the function can't change.

Together:

```calx
import std.io.{Lines}

// The words in `line`, which spaces separate.
fn words(line: String) -> List<String> {
    var out = List<String>()
    var start: usize = 0
    for i in 0..line.len() {
        if line.byte_at(i) == 32 {    // a space
            if i > start {
                out.push(line[start..i])
            }
            start = i + 1
        }
    }
    if start < line.len() {
        out.push(line[start..])
    }
    return out
}

fn main() {
    var input = Lines()
    var number = 0
    while let line = input.next() {
        number += 1
        let w = words(line)
        println("{number}: {w.len()} {w}")
    }
}
// stdin: add stone 64
// stdin: show
// stdin:
// out: 1: 3 [add, stone, 64]
// out: 2: 1 [show]
// out: 3: 0 []
```

Next: [what the commands mean](commands.md).
