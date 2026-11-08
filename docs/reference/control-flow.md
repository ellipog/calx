---
title: Control flow
description: Branches and loops - if, while, while let, for, loop, break and continue - and which of them give values. Patterns covers match.
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

## `while let`

`while let pattern = value { }` works `value` out before each pass, and runs the body while the pattern fits it, with the pattern's names bound for that pass. On an optional, the pattern matches the value inside, as [`if let`](patterns.md#if-let)'s does, so a queue drains with `while let x = q.pop_first()`:

```calx
fn main() {
    var queue = Deque<String>()
    queue.push_last(String("stone"))
    queue.push_last(String("dirt"))
    while let block = queue.pop_first() {
        if block == "stone" {
            queue.push_last(String("gravel"))
        }
        println(block)
    }
    // out: stone
    // out: dirt
    // out: gravel
    var stack: List<i32> = [1, 2, 3]
    var sum = 0
    while let x = stack.pop() {
        sum += x
    }
    println("{sum}")    // out: 6
}
```

- It's `loop { if let pattern = value { ... } else { break } }`, so `break` and `continue` work as in any loop, and `continue` works the value out again. It can't `break` with a value, as a `while` can't.
- A pattern that fits every value, such as `while let x = 5`, is an error, since the loop could only stop at a `break`: write `loop`, with `let` inside.
- Each pass owns what the pattern takes from a new value, such as the `String` that `pop_first` gives, and frees it at the end of the pass. Names bound from a variable are views into it, as in `if let`, so the body can't change the variable unless its type is `Copy`.

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
- The loop variable can't be assigned. `for _ in 0..n` needs no variable. To change the elements, loop with `in mut` ([below](#changing-the-elements)).
- `parallel for i in a..b` runs the iterations at once, on several threads, and so does `parallel for x in xs` over a list's or an array's elements: see [Parallel loops](parallel.md).

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

### Changing the elements

`for x in mut xs` goes over the elements of a list, an array or a deque, and `x` is a view of each one: changing `x` changes the element, as a [`mut` parameter](functions.md#parameter-modes) changes the caller's value (D116).

```calx
struct Mob {
    hp: i32,
    tags: List<String>,
}

fn main() {
    var xs: List<i32> = [1, 2, 3]
    for x in mut xs {
        x *= 10
    }
    var mobs = [Mob { hp: 5, tags: [] }, Mob { hp: 9, tags: ["boss"] }]
    for m in mut mobs {
        m.hp += 1
        m.tags.push("healed")
    }
    println("{xs} {mobs[0].hp} {mobs[1].tags}")    // out: [10, 20, 30] 6 [boss, healed]
}
```

- What the loop goes over is a variable or a part of one that can be changed: a `var`, a `mut` parameter, or a field or element of one.
- Nothing in the body reaches it except through `x`: reading it, as in `xs.len()` or `xs[0]`, is an error as well as changing it, so no element is read while it's half changed and the list never moves under the loop. As in [a loop over a field](#looping-over-a-field), the body can use the struct's other fields.
- `x` can be passed on as `mut x`, given a new value, which drops the old one, or changed in part, as in `m.tags.push(...)`. It can't move, since the element stays in the list: move a copy with `x.copy()`.
- It's an error if the body never changes `x`, as for a `mut` parameter: go over the elements without `mut`.
- The variable is one name. Take an element apart in the body, as in `let (a, b) = p`, or change its parts with `p.0 += 1`.
- A set's elements can't change where they are, since a change could move one, and a range's numbers can't change at all. Changing a map's values in place isn't built yet: change one with `m[k] = v`.

In C, `x` is a pointer to the element, and the list's length and memory are read once before the loop, since nothing in the body can change them.

### Counting, backwards, side by side, in steps

What a loop goes over can be counted, turned around, paired with another list or stepped (CLX-101):

```calx
fn main() {
    let xs: List<i32> = [10, 20, 30]
    let names: List<String> = ["ann", "bob"]
    for (i, x) in xs.enumerate() {
        println("{i}: {x}")
    }
    // out: 0: 10
    // out: 1: 20
    // out: 2: 30
    for (x, n) in xs.zip(names) {
        println("{n} has {x}")
    }
    // out: ann has 10
    // out: bob has 20
    var down: List<i32> = []
    for i in (0..10).step_by(3).rev() {
        down.push(i)
    }
    println("{down}")    // out: [9, 6, 3, 0]
    var last = String()
    for (i, x) in xs.enumerate().rev() {
        last.push_str("{i}:{x};")
    }
    println(last)        // out: 2:30;1:20;0:10;
}
```

| Written | Goes over |
| --- | --- |
| `xs.enumerate()` | `(i, x)`: each element with its count, a `usize` from 0. It works on anything a loop goes over, a range, a map or a text's runes included |
| `xs.rev()` | The elements from the last to the first, of a range, a list, an array or a zip |
| `xs.zip(ys)` | `(x, y)`: an element of each, side by side, from two lists or arrays of any element types, until the shorter one ends |
| `(a..b).step_by(n)` | `a`, `a + n`, `a + 2n` and on, up to `b`, of a range, as Python's `range(a, b, n)`. A step that isn't above 0 panics |

- They go in this order: `enumerate`, `rev`, then `zip` or `step_by` on what the loop goes over. `xs.enumerate().rev()` keeps each element's own index going backwards, and `xs.rev().enumerate()` counts the passes, 0 first.
- They're only what a `for` loop goes over, as `s.runes()` is: the loop is written for them, so nothing is made to go over, and the count is a number the loop keeps. Elsewhere, `xs.enumerate()` is an error.
- The elements are views, as in any loop over a list, and both lists of a zip are kept from changing until the loop ends. A zip's second list is a variable, or a part of one, unless its elements are `Copy`.
- `for x in mut xs.rev()` changes the elements from the last. To change them while counting, go over the indexes: `for i in 0..xs.len()`.
- A stepped range backwards starts at the last value its step reaches, so `(0..10).step_by(3).rev()` is 9, 6, 3, 0, as in Rust. Ranges up to the type's end stop without overflowing, with or without a step.

### Iterators

A type that implements `Iterator<T>` can be gone over too. `for x in it` calls `next` until it gives `none`:

```calx
struct Countdown {
    n: i32
}

impl Iterator<i32> for Countdown {
    fn next(mut self) -> i32? {
        if self.n == 0 {
            return none
        }
        self.n -= 1
        return some(self.n + 1)
    }
}

fn main() {
    var got: List<i32> = []
    for x in (Countdown { n: 3 }) {
        got.push(x)
    }
    println("{got}")    // out: [3, 2, 1]
    var c = Countdown { n: 5 }
    for x in c {
        if x == 4 {
            break
        }
    }
    println("{c.n}")    // out: 3
}
```

- `Iterator<T>` is built in, with one method, `fn next(mut self) -> T?`.
- What the loop goes over is a `var`, a `mut` parameter, a field of either, or a value made there, which the loop holds while it runs. A variable keeps what's left after a `break`. A struct literal in a loop's head goes in parentheses.
- Each value is the pass's own: the body can move it, and what it doesn't move is freed at the end of the pass. A pattern names every part that owns memory, as `(a, _rest)` does, so nothing is left without a name to free it by.
- A generic parameter with the bound `Iterator<T>` can be gone over: `fn total<I: Iterator<i32>>(it: mut I)`.
- `enumerate` counts an iterator's values. `rev`, `zip` and `step_by` need to know where the end is, so they don't go on an iterator.
- `parallel for` and `for x in mut` don't go over an iterator, which gives values one at a time rather than places.
- In C the loop calls `next` directly, as a function of the type's own, each pass, with no table or allocation in between.

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

Code after `return`, `break`, `continue`, a [panic](printing.md#panics), or a call to a function that [never returns](functions.md#functions-that-never-return), such as `std.process.exit`, can never run, so it's an error.

```calx
fn main() {
    return
    println("never")    // error: this code is never reached
}
```
