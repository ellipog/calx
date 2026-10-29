---
title: Functions
description: Declaring and calling functions, parameter modes, self, named arguments, returns, main, and functions as values.
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
- Within one call, a value passed as `mut` can't appear in any other argument, nor can any part of it (D48), so the views never overlap. The emitted C marks them `restrict`, and a function with a loop that's given a `mut` list or deque, or a struct that holds one, works on a copy of it in a local that it writes back as it returns, so clang keeps the list's fields in registers ([lists](lists.md#in-c), D159).
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

Arguments can be named, in any order after the unnamed ones: `area(height: 2, width: 3)`. Arguments run in the order they're written, whatever order the parameters are in. Default parameter values aren't built yet.

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

Closures, which capture values from where they're written, aren't built yet.

### In C

A function type is a C function pointer, `typedef cx_i32 (*cx_fn_i32__i32)(cx_i32);`, whose parameters are passed as a direct call passes them: views as pointers, everything else by value. So a call through a value costs one indirect call, and clang turns a call through a value it can see into a direct call.
