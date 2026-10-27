---
title: Functions
description: Declaring and calling functions, parameter modes, self, named arguments, returns, and main.
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
- Within one call, a value passed as `mut` can't appear in any other argument, nor can any part of it (D48), so the views never overlap. The emitted C marks them `restrict`.
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
