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
| `assert(cond)`, `assert(cond, s)` | Panics if `cond` is false, with the message `assertion failed: s` |
| `panic(s)` | Panics with the message `s` |

The argument is a `str`. A string literal there can interpolate: `{expr}` writes the value of `expr`. Integers, floats, `bool`, `char`, `rune`, `str` and [enums without data](enums.md#comparing-and-printing) can be interpolated. Floats print as Java prints them ([floats](floats.md#printing)), and enums print their variant's name. Interpolation elsewhere needs `String`, which isn't built yet.

Every interpolation runs, left to right, before anything is written, as Java builds the whole string first. So a function called inside `{...}` that prints writes its output before the line it's in.

Standard output is buffered and written at exit, or when a panic starts. It's the same bytes on Windows and Linux: `\n` is never turned into `\r\n`.

## Panics

A panic is an unrecoverable bug: an index out of bounds, a division by zero, `opt!` on `none`, a failed `assert`, or `panic(...)`. The program writes its output so far, then prints `panic at file:line:column: message` to standard error and exits with status 101.

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
