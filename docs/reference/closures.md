---
title: Closures
description: "Closures: |x| expr, Fn bounds, what a closure captures and can't do with it, trailing closures, the list and optional methods that take one, and their C."
---

# Closures

A closure is a function written where it's used, `|x| x * 2`, that can read the variables around it. It's passed to a parameter whose type is `Fn(A) -> R`, which the function calls.

```calx
fn apply(f: Fn(i32) -> i32, x: i32) -> i32 {
    return f(x)
}

fn main() {
    let step = 10
    println("{apply(|x| x + step, 5)}")    // out: 15
    let xs: List<i32> = [3, 1, 4, 1, 5]
    println("{xs.map(|x| x * step)}")       // out: [30, 10, 40, 10, 50]
    println("{xs.filter(|x| x > 2)}")       // out: [3, 4, 5]
}
```

- `|a, b| expr` takes two parameters and gives back `expr`. `|| expr` takes none. A body of more than one statement is a block, which ends with its value: `|x| { let y = x * 2; y + 1 }`.
- A parameter can say its type, `|x: i32| ...`. Without one, the type comes from where the closure is passed.
- A closure gives back what its body ends with. It can't `return`, `break`, `continue`, `try` or `fail`: those would leave the function around it.
- A closure's parameters are read, like a function's that have no mode ([parameter modes](functions.md#parameter-modes)), so they can't be `mut` or `own`.

## Fn bounds

`Fn(A, B) -> R` is what a closure is called as: it takes an `A` and a `B` and gives back an `R`. `Fn(A)`, with no `->`, gives back nothing.

```calx
fn count_if<T, F: Fn(T) -> bool>(xs: List<T>, keep: F) -> usize {
    var n: usize = 0
    for x in xs {
        if keep(x) {
            n += 1
        }
    }
    return n
}

fn each(xs: List<String>, f: Fn(String)) {
    for x in xs {
        f(x)
    }
}

fn main() {
    let names: List<String> = ["ann", "bob", "al"]
    println("{count_if(names, |s| s.starts_with("a"))}")    // out: 2
    let min: usize = 3
    println("{count_if(names, |s| s.len() >= min)}")        // out: 2
    each(names, |s| println("hi {s}"))
    // out: hi ann
    // out: hi bob
    // out: hi al
}
```

- `f: Fn(i32) -> i32` is short for a generic parameter with that bound, `<F: Fn(i32) -> i32>(f: F)`. Write the generic parameter when its type is named elsewhere too.
- Each closure has a type of its own, so a function that takes one is checked once and made again for each closure it's given, as any generic function is ([generics](generics.md)). The call inside it calls the closure's code directly, and clang puts it inline.
- A generic parameter can have one `Fn` bound, and no other bounds with it.
- The types in an `Fn` bound can be the function's generic parameters. The arguments before the closure say what they are, and the closure's body says what its result is: in `map_all(xs, |x| "n{x}")` below, `T` is `i32` from `xs`, and `U` is `String` from the body.
- What a parameter with an `Fn` bound holds can only be called, or passed on to another parameter with an `Fn` bound. It can't be stored, returned or put in a variable.
- A function can be passed where a closure is wanted, as in `apply(double, 4)`.

```calx
fn map_all<T, U, F: Fn(T) -> U>(xs: List<T>, f: F) -> List<U> {
    var out = List<U>()
    for x in xs {
        out.push(f(x))
    }
    return out
}

fn double(x: i32) -> i32 {
    return x * 2
}

fn main() {
    let xs: List<i32> = [1, 2, 3]
    println("{map_all(xs, |x| "n{x}")}")    // out: [n1, n2, n3]
    println("{map_all(xs, double)}")        // out: [2, 4, 6]
}
```

## Captures

A closure reads the variables it names from the code around it where they are, so nothing is copied into it. That's why it's only ever passed to a call: the variables it reads are still there until the call is done.

- It can't change them. Change a copy in its body, or use a `for` loop.
- It can't move them. Move a copy, as in `s.copy()`.
- Nothing can move them while the call it's passed to runs: `takes_both(|x| x + n.len(), n)`, which moves `n` into the call too, is an error.
- A list method that changes the list, such as `sort_by` or `retain`, can't be given a closure that reads the list.
- A closure inside a closure reads the outer one's parameters and captures the same way. One in a method reads `self`.

```calx
fn apply(f: Fn(i32) -> i32, x: i32) -> i32 {
    return f(x)
}

fn main() {
    var n = 1
    _ = apply(|x| { n = 2; x }, 1)    // error: a closure reads what it captures where it is, so it can't change `n`
}
```

## Closures as function values

A closure that captures nothing is a plain function, so it can be a `fn(...)` value: stored, returned or put in a variable ([functions as values](functions.md#functions-as-values)). Its parameters need types unless the value's type says them.

```calx
struct Command {
    name: str
    run: fn(i32) -> i32
}

fn main() {
    let square = |x: i32| x * x
    let commands = [Command { name: "inc", run: |x| x + 1 }, Command { name: "square", run: square }]
    for c in commands {
        println("{c.name}: {(c.run)(5)}")
    }
    // out: inc: 6
    // out: square: 25
}
```

One that captures something can't be a `fn(...)`, which holds nothing. Pass it to a parameter with an `Fn` bound instead.

## After the call

A closure passed as the last argument can follow the call's parentheses.

```calx
fn repeat_text(n: usize, f: Fn(usize) -> String) -> String {
    var s = String()
    for i in 0..n {
        s.push_str(f(i))
    }
    return s
}

fn main() {
    let dashes = repeat_text(3) |i| {
        "{i}-"
    }
    println(dashes)    // out: 0-1-2-
}
```

## Methods that take one

A list's methods that call a closure on each element, in order. Each also takes a function, or a parameter with an `Fn` bound.

| Method | Gives |
| --- | --- |
| `xs.map(\|x\| ...)` | A new list of what the closure gives back for each element |
| `xs.filter(\|x\| ...)` | A new list of copies of the elements it gives `true` for |
| `xs.retain(\|x\| ...)` | Keeps the elements it gives `true` for, in order, and frees the rest |
| `xs.any(\|x\| ...)`, `xs.all(\|x\| ...)` | Whether it gives `true` for any element, or for all of them. They stop at the first that decides |
| `xs.count(\|x\| ...)` | How many elements it gives `true` for, a `usize` |
| `xs.find(\|x\| ...)` | A copy of the first element it gives `true` for, or `none` |
| `xs.position(\|x\| ...)` | The index of the first element it gives `true` for, a `usize?` |
| `xs.sort_by(\|a, b\| ...)` | Sorts by what the closure gives back, an [`Ordering`](comparing.md#order), as `a.cmp(b)` does |
| `xs.sort_by_key(\|x\| ...)` | Sorts by the key the closure gives back, which must be `Copy` and have `Order` |
| `opt.map(\|x\| ...)` | On an optional: what the closure gives back for its value, or `none` |

- The closure reads each element in place. `filter` and `find` copy what they keep, so the elements can't hold a thread or another handle that can't be copied.
- `sort_by` and `sort_by_key` keep equal elements in the order they were in, so sorting by one key and then another works.
- `retain`, `sort_by` and `sort_by_key` change the list, so it must be a `var` or a `mut` parameter.
- An [array](types.md#arrays) has them all but `retain`, which would change its length. Its `map` gives an array of the same length.

```calx
struct Player {
    name: String
    score: i32
}

fn main() {
    var team: List<Player> = [Player { name: String("a"), score: 3 }, Player { name: String("b"), score: 9 },
                              Player { name: String("c"), score: 5 }]
    team.sort_by_key(|p| -p.score)
    println("{team.map(|p| p.name.copy())}")                  // out: [b, c, a]
    println("{team.find(|p| p.score < 5).map(|p| p.score)}")  // out: some(3)
    var words: List<String> = ["pear", "fig", "banana", "kiwi"]
    words.sort_by(|a, b| a.len().cmp(b.len()))
    println("{words}")                                        // out: [fig, pear, kiwi, banana]
    words.retain(|w| w.len() == 4)
    println("{words} {words.any(|w| w.starts_with("k"))}")    // out: [pear, kiwi] true
}
```

## In C

- A closure is a `static` C function in its module's file. Its first parameter, `const void* const* cx_env`, is an array of the addresses of what it captures, which the call builds on the stack. A closure that captures nothing gets a null pointer, and one used as a `fn(...)` value has no such parameter.
- Since a function with an `Fn` bound is made for each closure, the call in it is a direct call to the closure's function, with the addresses as the first argument, and clang puts it inline. A `map` over a list runs as fast as the loop written out.
- The list methods are loops inline at the call. `sort_by` and `sort_by_key` use the file's own stable sort from `calx_sort.h`, made for each closure, with the closure as its comparison.
- What a closure captures it only reads, and nothing moves or changes it until the call is done, so the addresses stay valid. In the closure's function each is a `restrict` pointer to a value nothing changes.
