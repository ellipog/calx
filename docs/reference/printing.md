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

The argument is a `str` or a `String`. A string literal there can interpolate: `{expr}` writes the value of `expr`, with no string built in between. Every value can be interpolated except a pointer, and a generic parameter without the bound `Display`: numbers, `bool`, text, and structs, enums, tuples, arrays, optionals and lists of them ([comparing and printing](comparing.md#display)). Floats print as Java prints them ([floats](floats.md#printing)), and a struct prints as its literal, such as `Point { x: 1, y: 2 }`, unless it [writes its own `Display`](comparing.md#writing-your-own). An interpolating literal anywhere else builds a `String` ([strings](strings.md#literals)).

Every interpolation runs, left to right, before anything is written, as Java builds the whole string first. So a function called inside `{...}` that prints writes its output before the line it's in.

A value that's `Copy`, such as a number, is copied when its `{...}` runs. A variable, field or element that owns memory, such as a list or a map, is written as it is once they've all run, so a later `{...}` can't change it or move it, which is a compile error. Work that one out first, into a variable. Reading from it, as `{xs.len()}` does, holds nothing:

```calx
fn main() {
    var xs: List<i32> = [1, 2, 3]
    println("{xs.len()} {xs.pop()} {xs}")    // out: 3 some(3) [1, 2]
    let last = xs.pop()
    println("{xs} {last}")    // out: [1] some(2)
}
```

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

### Stack traces

After the message, a panic writes the Calx functions it was in, innermost first, each at the line it had reached, as Java writes a stack trace (D120):

```calx
fn half(n: i32) -> i32 {
    assert(n % 2 == 0, "{n} is odd")    // panic: assertion failed: 7 is odd
    return n / 2
}

fn main() {
    println("{half(8)}")    // out: 4
    println("{half(7)}")
}
```

Saved as `example.calx` and run, that writes:

```text
4
panic at example.calx:2:5: assertion failed: 7 is odd
	at example.half(example.calx:2)
	at example.main(example.calx:8)
```

- A frame is the function's module and name, with a method's type between them, such as `world.Chunk.block`. A generic function has one name, whatever it was called with.
- A call the optimizer inlined is a frame of its own, so a trace is the same in debug and release builds, on Windows and on Linux.
- Only Calx functions are written, the standard library's too, not the runtime's, the C library's or the system's.
- A file inside the folder the program runs in is written relative to it, as in the message; any other in full.
- A [`@track_caller`](#panics-that-name-the-caller) function's message names its caller, and its trace still starts in the function, at its own line.
- Inside a `parallel for`, the trace is the one a single thread running the loop would write: the body at its line, the loop at its line, then out to `main`, loops inside loops too, whichever threads ran what. Each thread writes the part of it that's on its stack.
- A panic in a [library](libraries.md) is the program's, and its trace goes through both, the library's functions under their module's name.
- The program reads them from its own line tables when it panics. Every build has those, and they don't change the generated code, so until a panic a trace costs nothing. On Windows they're in a PDB, which `calx build` puts beside the program ([command](command.md)). It looks at the innermost 128 calls, not counting inlined ones, so the trace of a panic deep in a recursion stops before `main`.

### Panics that name the caller

A function marked `@track_caller` reports its panics at the place that called it, not at its own lines (D115). That's for a library's checks, such as an index that's out of range for a grid, where the bug is in the code that called it:

```calx
struct Grid {
    cells: List<i32>
    width: usize
}

impl Grid {
    @track_caller
    fn at(self, x: usize, y: usize) -> i32 {
        if x >= self.width {
            panic("x is {x}, but the grid is {self.width} wide")
        }
        return self.cells[y * self.width + x]
    }
}

fn main() {
    let g = Grid { cells: [1, 2, 3, 4, 5, 6], width: 3 }
    println("{g.at(2, 1)}")    // out: 6
    println("{g.at(3, 0)}")    // panic: x is 3, but the grid is 3 wide
}
```

- Every panic in the function names its caller: `panic`, a failed `assert`, an index out of bounds, `!` on `none`.
- A `@track_caller` function that calls another passes its own caller on, so a check two calls deep still names the code outside them.
- A `parallel for` inside one names its own places, since its body runs apart from the call.
- It's for functions and a type's own methods. A method of an interface's impl can't have it, since the interface's callers don't pass a place, and neither can `main`, since no Calx code calls it.
- In C, the function takes the place as a last parameter, a string such as `"src/main.calx:20:16"`, and its panics use that string, so it costs one pointer per call.

`@track_caller` and [`@export`](libraries.md#building-a-library) are the only attributes the compiler knows. Any other is an error, until libraries can define their own ([spec](../CALX_SPEC.md#library-defined-attributes)).
