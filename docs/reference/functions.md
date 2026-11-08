---
title: Functions
description: Declaring and calling functions, parameter modes, self, named arguments, default values, returns, main, and functions as values.
---

# Functions

```calx
fn area(width: i32, height: i32) -> i32 {
    return width * height
}

fn grow(size: mut i32, by: i32) {
    size += by
}

fn main() {
    var side = 3
    grow(mut side, 2)
    println("{area(side, 4)} {area(height: 2, width: side)}")    // out: 20 10
}
```

- `fn name(params) -> T { }` declares a function. Without `->`, it returns nothing.
- Functions can be declared in any order.
- `main`, with no parameters and no result, is where a program starts.

## Returning

A function gives back its value with `return value`, and must return on every path. A function's last line can't be a bare value, unlike a block used as a value, so a function always says where it returns.

A function that can fail declares its error type after `!`, as in `fn parse(s: str) -> i32 ! ParseError`, and returns its error with `fail` ([Errors](errors.md)).

```calx
fn double(x: i32) -> i32 {
    x * 2      // error: a function gives back its value with `return`
}

fn main() {
    println("{double(2)}")
}
```

### Functions that never return

`-> never` says a function never returns, as `panic` doesn't: it ends the program, or loops for good. Its body must end in a `panic`, a `loop` without `break`, or a call to another function that returns `never`, such as [`std.process.exit`](../std/process.md#this-process), and it can't `return`.

A call to one has the type `never`, which fits wherever a value goes, so it can end a function that returns a value, end a `match` arm, or stand in for a value, as in `x ?? die("...")`. Code after a call to one can never run, so it's an error, as it is after `return`.

```calx
import std.process.{exit}

// Says what's wrong and ends the program, as a script's `sys.exit("...")` does.
fn die(why: String) -> never {
    eprintln("tool: {why}")
    exit(2)
}

fn level(text: String) -> i32 {
    match i32.parse(text) {
        some(n) => return n,
        none => die("{text} isn't a level"),
    }
}

fn main() {
    println("{level(String("3"))}")    // out: 3
    let name: String? = none
    let shown = name ?? die("no name was given")
    println(shown)
}
// stderr: tool: no name was given
// status: 2
```

`never` is only what a function returns: a parameter, a variable or a list can't have it. A function that can fail can't return it either, since failing is returning. In C the function is `noreturn`, so clang knows the code after a call isn't reached.

## Parameter modes

| Mode | Written | The function gets | The caller passes |
| --- | --- | --- | --- |
| Read | `x: T` | A read-only view, or a copy for numbers and other small `Copy` values | `value` |
| Mutate | `x: mut T` | A mutable view: changes reach the caller's value | `mut value` |
| Take | `x: own T` | The value itself, which it now owns and can change. A value that owns memory moves in ([ownership](ownership.md)) | `value` |

- A read parameter can't be changed. A `mut` parameter must be changed somewhere, or it's an error.
- A read or `mut` parameter is a view of the caller's value, so it can't be moved, returned or stored. A function that keeps what it's given takes it as `own`.
- A method takes its value the same three ways: `self`, `mut self` and `own self`.
- `mut` at the call site shows which arguments can change. Only a variable, or a field or element of one, can be passed as `mut`.
- Within one call, a value passed as `mut` can't appear in any other argument, nor can any part of it (D48), so the views never overlap. The emitted C marks them `restrict`, and a function with a loop that's given a `mut` list, deque or `String`, or a struct that holds one, works on a copy of it in a local that it writes back as it returns, so clang keeps the list's fields in registers ([lists](lists.md#in-c), D159).
- Separate fields are separate values, so `mut w.blocks[k]` and `mut w.states[k]` can go in one call, and so can `mut s.due` and `s.scheduled`. Two elements of one list can't, as in `mut xs[i]` and `xs[j]`, since `i` and `j` may be equal.

```calx
struct Pair {
    a: i32
    b: i32
}

fn copy_into(to: mut Pair, from: Pair) {
    to.a = from.a
    to.b = from.b
}

fn main() {
    var p = Pair { a: 1, b: 2 }
    copy_into(mut p, p)    // error: `p` is passed as `mut`, so it can't be used again in the same call
}
```

Fields apart are fine:

```calx
struct Pair {
    a: i32
    b: i32
}

struct Two {
    first: Pair
    second: Pair
}

fn copy_into(to: mut Pair, from: Pair) {
    to.a = from.a
    to.b = from.b
}

fn main() {
    var t = Two { first: Pair { a: 1, b: 2 }, second: Pair { a: 0, b: 0 } }
    copy_into(mut t.second, t.first)
    println("{t.second.a} {t.second.b}")    // out: 1 2
}
```

## Named arguments

Arguments can be named, in any order after the unnamed ones: `area(height: 2, width: 3)`. Arguments run in the order they're written, whatever order the parameters are in.

## Default values

A parameter can have a default, `level: u32 = 6`, which a call that leaves it out passes. Naming the arguments skips some defaults and gives later ones, so one function serves where Java would overload it.

```calx
enum Align {
    Left,
    Right,
}

fn pad(text: str, width: usize = 8, fill: rune = ' ', align: Align = Align.Right) -> String {
    return match align {
        Align.Left => text.pad_end(width, fill),
        Align.Right => text.pad_start(width, fill),
    }
}

fn main() {
    println("[{pad("ab")}]")                           // out: [      ab]
    println("[{pad("ab", 4)}]")                        // out: [  ab]
    println("[{pad("ab", fill: '.', width: 5)}]")      // out: [...ab]
    println("[{pad("ab", align: Align.Left)}]")        // out: [ab      ]
}
```

- A default is a constant: a literal, a [constant](variables.md#constants), `none`, an enum's variant without data, or an array or tuple of them. It sees only names at the module's top level, not the other parameters, and it's worked out where the function is declared, once.
- The parameters with defaults come last.
- A parameter with a default has a `Copy` type, a `String`, whose default points at the program's text, or an optional of one, or it's a list whose default is `[]`, which holds nothing. So a call that passes the default allocates nothing.
- A `mut` parameter can't have a default, since it changes the caller's variable, and neither can one whose type is generic, nor an interface's functions.
- A function value has no defaults: `let f = pad` takes all four arguments.

## Evaluation order

Calx evaluates operands and arguments left to right, as Java does, including the cases C leaves unspecified. In `f(a(), b())` and `a() + b()`, `a` always runs before `b`. In `x += f()`, the old value of `x` is read before `f` runs.

## Functions as values

A function's name without a call is the function as a value (D122). Its type is written as the function is declared, without the names: `fn(i32) -> i32`, `fn(mut List<i32>, own String)`, `fn(str) -> u32 ! ParseError`. The modes are part of the type, so a `fn(mut List<i32>)` and a `fn(List<i32>)` are different types.

```calx
fn double(x: i32) -> i32 {
    return x * 2
}

fn square(x: i32) -> i32 {
    return x * x
}

struct Command {
    name: str
    run: fn(i32) -> i32
}

fn main() {
    let commands = [Command { name: "double", run: double }, Command { name: "square", run: square }]
    for c in commands {
        println("{c.name}: {(c.run)(5)}")
    }
    // out: double: 10
    // out: square: 25
    let f = double
    println("{f(21)}")    // out: 42
}
```

- A value of a function type is called like a function, `f(x)`. Its arguments follow the same rules as a direct call's: `mut` ones are passed as `mut value`, `own` ones move in, and none overlap (D48). They have no names, and none can be left out.
- `s.f(x)` calls a method. To call a function held in a field, put the field in parentheses: `(s.f)(x)`.
- A function from an impl is a value too, with what it's called on first: `Point.len` is a `fn(Point) -> f64`, and `Point.origin` a `fn() -> Point`.
- A generic function's types come from `f<...>`, as in `first<u8>`, or from the type it's given as, as in `let f: fn(List<i32>) -> i32 = first`.
- An `unsafe fn`, or a C function from an `extern` block, has an `unsafe fn(...)` type, which only `unsafe` code can call. Pointers can appear only in `unsafe fn` types.
- A function that can fail gives a result from each call through its value, handled as any other call's is ([errors](errors.md)).
- A function value holds no data, so it's `Copy` and `Send`: it can be stored, copied, sent to another thread or called in a `parallel for`.
- Function values can't be compared, shown or hashed. Two functions with the same code can be one function in the program, so their addresses can't tell them apart.
- A `@track_caller` function can only be called, since its panics name the place that called it.
- `fn(i32) -> i32?` is a function that gives back an optional. An optional function is `(fn(i32) -> i32)?`.

A [closure](closures.md), `|x| x + n`, is a function written where it's used, which reads the variables around it. One that captures nothing is a plain function value too.

### In C

A function type is a C function pointer, `typedef cx_i32 (*cx_fn_i32__i32)(cx_i32);`, whose parameters are passed as a direct call passes them: views as pointers, everything else by value. So a call through a value costs one indirect call, and clang turns a call through a value it can see into a direct call.
