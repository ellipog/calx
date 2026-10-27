---
title: Control flow
description: if, while, for, loop, break and continue, and which of them give values. match is in Patterns.
---

# Control flow

## `if`

```calx
fn describe(n: i32) -> str {
    if n < 0 {
        return "negative"
    } else if n == 0 {
        return "zero"
    }
    return "positive"
}

fn main() {
    let n = 7
    let parity = if n % 2 == 0 { "even" } else { "odd" }
    println("{describe(n)} {parity}")    // out: positive odd
}
```

- The condition is a `bool`. Nothing else converts to one.
- `if` is an expression. Used as a value, it needs an `else`, and both branches give the same type. A block's value is its last line.
- `else` can start the line after the `}`.
- A literal `true` or `false` as the condition is an error, since one branch could never run.
- `if let pattern = value` runs the block when a pattern fits, and unwraps optionals ([`if let`](patterns.md#if-let)). `match` picks between many patterns ([patterns](patterns.md)).

## `while`

`while cond { }` runs the body while `cond` is true. `while true` is an error: write `loop`.

## `for`

```calx
fn main() {
    var sum = 0
    for i in 0..4 {          // 0, 1, 2, 3
        sum += i
    }
    for i in 1..=3 {         // 1, 2, 3
        sum += i * 10
    }
    let xs = [5, 6]
    for x in xs {
        sum += x
    }
    println("{sum}")         // out: 77
}
```

- `for i in a..b` counts from `a` up to but not including `b`, and `a..=b` includes `b`. Both ends are integers of the same type, and are evaluated once. `a..=MAX` stops at `MAX` without overflowing.
- `for x in array` goes over an array's elements in order. The array can't be changed during the loop.
- The loop variable can't be assigned. `for _ in 0..n` needs no variable.
- `parallel for i in a..b` runs the iterations at once, on several threads: see [Parallel loops](parallel.md).

### Looping over a field

What a loop goes over can't change or move until the loop ends, and neither can what holds it or what's in it. A loop over a field, such as `for t in r.scheduled`, keeps only that field: the body can change `r`'s other fields, which the loop never reads (D106).

```calx
struct Region {
    scheduled: List<i32>,
    due: List<i32>,
}

fn main() {
    var r = Region { scheduled: [4, 1, 3], due: List() }
    for t in r.scheduled {
        if t < 4 {
            r.due.push(t)    // another field of `r`
        }
    }
    println("{r.due}")    // out: [1, 3]
}
```

Changing `r.scheduled`, or `r` as a whole, is still an error in the loop: `r.scheduled` can't be changed while a `for` loop goes over it, and `r` can't be changed while a `for` loop goes over `r.scheduled`. Past an index, the loop keeps the whole list: in a loop over `rs[i].scheduled`, `rs[j].due` could be the same region's, so no element of `rs` can change.

## `loop`

`loop { }` runs until a `break`. Used as a value, it gives the value of `break value`.

```calx
fn main() {
    var n = 1
    let first = loop {
        n *= 3
        if n > 50 {
            break n
        }
    }
    println("{first}")    // out: 81
}
```

## `break` and `continue`

`break` leaves the innermost loop, and `continue` starts its next iteration. Only `loop` can break with a value.

## Unreachable code

Code after `return`, `break`, `continue` or a [panic](printing.md#panics) can never run, so it's an error.

```calx
fn main() {
    return
    println("never")    // error: this code is never reached
}
```
