---
title: Printing and panics
description: print and println with interpolation, assert, panic, and what a panic does.
---

# Printing and panics

```calx
fn main() {
    let name = "Steve"
    let hp = 18
    print("{name} has ")
    println("{hp} health, {hp * 5}%")    // out: Steve has 18 health, 90%
    println()                            // out:
    assert(hp > 0, "{name} is dead")
}
```

| Function | Does |
| --- | --- |
| `print(s)` | Writes `s` to standard output |
| `println(s)`, `println()` | Writes `s`, then a newline |
| `eprint(s)`, `eprintln(s)`, `eprintln()` | The same, to standard error |
| `assert(cond)`, `assert(cond, s)` | Panics if `cond` is false, with the message `assertion failed: s` |
| `panic(s)` | Panics with the message `s` |

The argument is a `str` or a `String`. A string literal there can interpolate: `{expr}` writes the value of `expr`, with no string built in between. Integers, floats, `bool`, `char`, `rune`, `str`, `String`, [enums without data](enums.md#comparing-and-printing) and [lists](lists.md#printing) of them can be interpolated. Floats print as Java prints them ([floats](floats.md#printing)), and enums print their variant's name. An interpolating literal anywhere else builds a `String` ([strings](strings.md#literals)).

Every interpolation runs, left to right, before anything is written, as Java builds the whole string first. So a function called inside `{...}` that prints writes its output before the line it's in.

Standard output is buffered and written at exit, or when a panic starts. It's the same bytes on Windows and Linux: `\n` is never turned into `\r\n`.

Standard error isn't buffered. `eprint` and `eprintln` first write out whatever standard output has waiting, so when both go to one terminal they appear in the order they were written. A program whose standard output is a protocol, such as the rig's adapter, logs with them. [`std.io`](../std/io.md) reads and writes bytes.

## Panics

A panic is an unrecoverable bug: an index out of bounds, a division by zero, `opt!` on `none`, a failed `assert`, or `panic(...)`. The program writes its output so far, then prints `panic at file:line:column: message` to standard error and exits with status 101.

Inside a `parallel for`, what's printed and which panic is reported are the same as if the iterations ran one after another ([parallel loops](parallel.md#the-same-result-with-any-number-of-threads)).

```calx
fn main() {
    let xs = [1, 2, 3]
    var i: usize = 0
    i += 5
    println("{xs[i]}")    // panic: index 5 is out of bounds for length 3
}
```

`panic(...)` never returns, so code after it is unreachable, and it fits wherever a value is needed:

```calx
fn check(n: i32) -> i32 {
    let half = if n % 2 == 0 { n / 2 } else { panic("{n} is odd") }
    return half
}

fn main() {
    println("{check(8)}")    // out: 4
}
```
