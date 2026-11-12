---
title: Printing and panics
description: Writing output with print and println and their interpolation, formats such as {x:.2} and {n:>8}, checking with assert, and what panic does.
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

## Formats

`{x:spec}` says how the value is written: padded to a width, rounded to some digits, in hex, grouped in thousands, and so on. The format is written as Rust's and Python's are:

```calx
fn main() {
    let price = 1234.5
    let n = 255
    println("[{price:.2}] [{price:>10.1}] [{price:,.2}] [{price:e}] [{0.256:.1%}]")    // out: [1234.50] [    1234.5] [1,234.50] [1.2345e3] [25.6%]
    println("[{n:x}] [{n:#06x}] [{n:b}] [{n:>6}] [{n:<6}] [{n:^7}] [{n:*>6}] [{n:+}] [{1234567:_}]")    // out: [ff] [0x00ff] [11111111] [   255] [255   ] [  255  ] [***255] [+255] [1_234_567]
    let name = "Steve"
    println("[{name:>8}] [{name:-^9}] [{name:.3}]")    // out: [   Steve] [--Steve--] [Ste]
}
```

`[[fill]align][+][#][0][width][,|_][.precision][kind]`, each part optional, in that order:

| Part | Means |
| --- | --- |
| `<`, `>`, `^` | Left, right or centred in the width. Numbers go right and everything else left unless the format says |
| A rune before the alignment | The fill, a space unless given: `{x:*^9}`, `{x:·>8}` |
| `+` | A sign on numbers that aren't negative too |
| `#` | `0x`, `0o` or `0b` before hex, octal or binary digits |
| `0` | Zeros after the sign and the prefix, up to the width: `{-5:04}` is `-005` |
| A width | The least runes to write, as many as the value needs if it's longer |
| `,` or `_` | A separator between groups of three digits, or of four in hex, octal and binary |
| `.N` | For a float, N digits after the point. For text, the most runes to write |
| `x`, `X`, `o`, `b` | An integer in hex, in hex with capitals, in octal or in binary |
| `e`, `E` | A float in scientific notation, `1.5e3` |
| `%` | A float a hundred times larger, with `%` after it |

- Each type takes the parts that mean something for it, and the checker says when a part doesn't: an integer takes everything but `.N`, `e` and `%`; a float everything but `#` and the integer kinds; text a fill, an alignment, a width and `.N`; and any other value, a `bool`, a rune, an enum, a struct, a list, an optional, a fill, an alignment and a width.
- A negative integer in hex, octal or binary is written as its bits, as Java's `Integer.toHexString` writes it: `{-1:x}` on an `i32` is `ffffffff`, and on an `i8`, `ff`.
- `{x:.2}` rounds the float's exact value, ties to even, as Rust, Python and C do. So `{0.125:.2}` is `0.12`, and `{2.675:.2}` is `2.67`, since the double nearest 2.675 is a little less than it. Without `.N`, a float is written as `{x}` writes it, the shortest digits that read back.
- `{x:e}` writes the shortest digits in scientific notation, and `{x:.3e}` rounds to three digits after the point. The exponent is written as Rust writes it, with no `+` and no leading zeros: `1.5e3`, `2e-7`. The digits are Rust's and Python's `repr`'s, so the smallest double is `5e-324`, where `{x}` writes Java's `4.9E-324` ([floats](floats.md)).
- `{x:%}` is the shortest digits moved two places, `12.5%`, and `{x:.1%}` rounds the exact value times 100, `12.3%`. Python multiplies by 100 in floating point first, which rounds again, so for a huge value its digits can differ.
- `NaN` and `Infinity` take a fill and a width but no zeros, and `NaN` takes no sign.
- `,` groups the digits before the point, Python's way: `{1234.5:,.2}` is `1,234.50`, and `{1234:08,}` is `0,001,234`.
- Widths count runes, not bytes, so `{"é":3}` is `é` and two spaces.
- A `:` outside brackets ends the expression, so an expression with one of its own, such as a closure with typed parameters, goes in parentheses.
- What isn't a format is an error that says what to write instead: `{x:.2f}` says the `f` isn't needed, `{x:?}` that `{x}` writes any value, and `{x:g}` which formats give the digits it would.

Formats print as the rest of the string does, so `"{x:.2}"` builds a `String` and `s.push_str("{x:.2}")` adds to one. `s.push_str("..{x}..")` prints its parts straight into `s`, as Rust's `write!` does, with no string built and copied between, whether or not they have formats.

The formats are in [runtime/calx_format.c](../../runtime/calx_format.c), and the float ones in [calx_float.c](../../runtime/calx_float.c), which rounds with one 128-bit multiply when there are at most 19 digits after the point and the value fits in 64 bits, and with exact big integers otherwise. A padded value is put together on the stack and written once. A number padded on the left, with no sign, zeros or groups asked for, as `{n:>8}`, gets its fill straight before its digits where they were written, so nothing is copied: that took `{i:>8}` from 3.3 ms to 1.5 ms below. They're linked only into programs that use them. `{x:.p}`, `.pe` and `.p%` matched Python's exact formatting on 32,715 doubles: random bits, decimals, exact halves, subnormals, and the neighbours of every power of ten.

Into a `String`, on our benchmark, an i9-13900K otherwise quiet, each program on its performance cores, the best of 42 runs taken in turns:

| | Calx | Rust |
| --- | --- | --- |
| 200,000 doubles, `"{v:.3},"` | 7.8 ms | 14.3 ms |
| 200,000 integers, `"{i:>8},"` | 1.5 ms | 3.0 ms |
| A million integers, `"{i},"` | 5.7 ms | 11.2 ms |
| 200,000 doubles, `"{v},"` | 5.0 ms | 10.2 ms |

The Rust is Rust 1.91's `write!(out, "{:.3},", v)` and the same for the rest, into a `String`, built for release with `x86-64-v3`. The values are a xorshift's: doubles from 0 to 1,000 and integers below 1,048,576. Calx writes integers two digits at a time from a table, as Rust does.

Standard output is buffered and written at exit, or when a panic starts. It's the same bytes on every platform: `\n` is never turned into `\r\n`.

Standard error isn't buffered. `eprint` and `eprintln` first write out whatever standard output has waiting, so when both go to one terminal they appear in the order they were written. A program whose standard output is a protocol, such as the rig's adapter, logs with them. [`std.io`](../std/io.md) reads and writes bytes.

## Panics

A panic is an unrecoverable bug: an index out of bounds, a division by zero, `opt!` on `none`, a failed `assert`, a call deeper than the stack holds, or `panic(...)`. The program writes its output so far, then prints `panic at file:line:column: message` to standard error and exits with status 101.

Every thread's stack is 8 MiB ([std.thread](../std/thread.md), D135). A call that would go past its end panics like the rest, on any thread and in any build. That's no one line's fault, so the place is `<stack>`, and the [trace](#stack-traces) shows the calls that got there:

```text
panic at <stack>: the calls went deeper than the thread's 8 MiB stack holds
	at example.walk(example.calx:4)
	at example.walk(example.calx:9)
	... repeated 126 more times
```

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
- A frame that comes again right after itself, as a recursion's does, is written once, then `... repeated N more times`. Once more is just written again.
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
- It's for functions, a type's own methods, and the impls of the [operator interfaces](interfaces.md#operators), `Add`, `Sub`, `Mul`, `Div`, `Rem` and `Neg`, and of [`Index` and `SetIndex`](interfaces.md#indexing), so a struct's `a / b` can name the line that divides, and its `g[p]` the line that reads past its end. Every use of an operator is a call of the impl the checker finds, in a generic function too, where the place is the line in that function. The functions of other interfaces' impls can't have it, since the code the compiler writes calls them too, for a map's hashing or a string's `Display`, with no place to give. Neither can `main`, since no Calx code calls it.
- In C, the function takes the place as a last parameter, a string such as `"src/main.calx:20:16"`, then a zero byte and the name of the function the call is in. Its panics use the string up to the zero, so it costs one pointer per call.

#### Reading the caller

Inside a `@track_caller` function, `caller()` is the place that called it, as a `Caller` (CLX-175). That's for code that reports where it was used without panicking, such as a log line or a test helper's message. [std.log](../std/log.md) writes Python's `%(lineno)d` and `%(funcName)s` from it.

```calx
@track_caller
fn expect(holds: bool, what: String) {
    if not holds {
        let at = caller()
        println("{what} failed in {at.function}, at column {at.column}")
    }
}

fn check_sizes() {
    expect(2 + 2 == 4, "adding")
    expect(3 > 4, "comparing")    // out: comparing failed in check_sizes, at column 5
}

fn main() {
    check_sizes()
}
```

| Field | Is |
| --- | --- |
| `file: String` | The source file's path, as the program was built from it and as its panics name it: `src/main.calx` |
| `line: u32`, `column: u32` | Where the call starts, from 1 |
| `function: String` | The name of the function the call is in, such as `main` or a method's name, or `<closure>` in a closure |

- A `Caller` shows as panics name a place: `src/main.calx:20:16`.
- A `@track_caller` function that calls another passes its caller on, so `caller()` two calls deep is still the code outside them.
- `caller()` is an error outside a `@track_caller` function, and in a closure or a `parallel for` inside one, which run as functions of their own. Read it before them.
- A `caller` or a `Caller` of the program's own comes first, so neither name is taken from a program that has one.

`@track_caller`, [`@export`](libraries.md#building-a-library), [`@handle`](ownership.md#handles), [`@test`](command.md#testing) and `@attribute` are the attributes the compiler knows. Any other is a function a library marks `@attribute` ([attributes](attributes.md)), and one that's neither is an error.
