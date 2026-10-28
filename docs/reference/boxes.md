---
title: Boxes
description: Box<T>, one value on the heap, so a type can hold itself - making one, its value as the place b.value, what it can't do, and its C.
---

# Boxes

`Box<T>` owns one value on the heap. A type can't hold itself, since it would have no end, but it can hold a box of itself, since a box keeps its value on the heap, so it's the same size however big what it holds is. A list of itself works too, as a list's elements are on the heap ([generics](generics.md#generic-structs-and-enums)), and a box is for one:

```calx
enum Expr {
    Num(i64)
    Add(Box<Expr>, Box<Expr>)
    Neg(Box<Expr>)
}

fn eval(e: Expr) -> i64 {
    return match e {
        Expr.Num(v) => v,
        Expr.Add(a, b) => eval(a.value) + eval(b.value),
        Expr.Neg(x) => -eval(x.value),
    }
}

fn main() {
    let e = Expr.Add(Box(Expr.Num(2)), Box(Expr.Neg(Box(Expr.Num(5)))))
    println("{eval(e)}")    // out: -3
}
```

`Box(v)` makes a box holding `v`, and `Box<T>(v)` names what it holds. An optional box is how a chain ends:

```calx
struct Node {
    value: i32,
    next: Box<Node>?,
}

fn sum(n: Node) -> i32 {
    var total = n.value
    if let next = n.next {
        total += sum(next.value)
    }
    return total
}

fn main() {
    var head = Node { value: 0, next: none }
    for i in 1..5 {
        head = Node { value: i, next: some(Box(head)) }
    }
    println("{sum(head)}")    // out: 10
}
```

## The value

`b.value` is the value's place, as a field is: it's read, changed, passed as a `mut` view and matched where it is. A box is freed with its value, copied with `copy()` into a box of its own, compared and hashed as its value, and shown as its value:

```calx
struct Cell {
    name: String,
    hits: i32,
}

fn hit(c: mut Cell) {
    c.hits += 1
}

fn main() {
    var b = Box(Cell { name: String("stone"), hits: 0 })
    b.value.hits = 5
    hit(mut b.value)
    let c = b.copy()
    b.value.name.push_str("_bricks")
    println("{b.value.name} {b.value.hits} {c.value.name}")    // out: stone_bricks 6 stone
    println("{Box(3) == Box(3)} {Box(String("x"))}")    // out: true x
}
```

A value that owns memory can't be moved out of its box, which keeps it: `b.value.copy()` is a copy of it.

## What a box can't do

A box isn't a list, though it's one in memory:

- It isn't indexed, gone over in a loop, or written as a list literal: its value is `b.value`, and a box is made with `Box(v)`.
- It has no methods and no fields but `value`: `b.value.len()` is the value's.
- It has no `JavaHash`, since Java has no boxes, so it isn't a key of a `JavaHashMap`. A native `Map`'s key can be one.

## In C

A box is a list of its one value: the same pointer, length and capacity, so it's freed, copied, compared and hashed by the same code. `b.value` is the element at index 0, read without a bounds check, since it's always there.
