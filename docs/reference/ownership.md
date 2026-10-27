---
title: Ownership
description: Copy and owning values, moves, views, partial moves, patterns that take a value apart, when values are freed, and the C it all compiles to.
---

# Ownership (D76)

Every value has one owner: a variable, a parameter taken with `own`, a field, or an element of a list. A value that owns memory, such as a `String` or a `List`, is freed when its owner ends. There's no garbage collector and no reference counting, and the compiler works out where each value is freed, so a program can't leak, free a value twice, or use one after it's freed.

```calx
fn shout(s: String) -> String {
    return "{s}!"
}

fn main() {
    let a = String("hello")
    let b = a                 // `a` moves into `b`
    println(shout(b))         // out: hello!
    let n = 5
    let m = n                 // numbers are `Copy`, so `n` keeps its value
    println("{n} {m}")        // out: 5 5
}                             // `b` is freed here
```

## `Copy` values and owning values

A type is `Copy` when its values own no memory. Copying one copies its bytes, and both copies can be used.

- `Copy`: the integers, floats, `bool`, `char`, `rune` and `str`, enums without data, and arrays, tuples, optionals, structs and enums whose parts are all `Copy`.
- Owning: `String`, `List<T>`, and anything that holds one.
- A generic parameter is `Copy` only with the bound `T: Copy` ([generics](generics.md#values-of-type-t)).

Nothing marks a type `Copy`: it follows from what the type holds. A struct that gains a `String` field stops being `Copy`, and the compiler then checks every use of it as an owning value.

## Moves

An owning value moves when it's given a new owner:

- assigned to a variable or a field: `let b = a`, `p.name = s`
- passed to an `own` parameter
- returned
- put in a struct, tuple, list, optional or variant: `Pair { a: s }`, `some(s)`, `xs.push(s)`

After a move, the variable it was in can't be used, until it's assigned a new value:

```calx
fn main() {
    let a = String("x")
    let b = a
    println("{a} {b}")    // error: `a` was moved, so it can't be used here
}
```

A value moved on only some paths, such as inside an `if`, might be gone afterwards, so it can't be used after either. It's freed at the end of its scope only if it's still there.

A move inside a loop is an error unless the variable gets a new value before the loop comes around, since the next pass would find it gone.

`x.copy()` makes a copy that owns memory of its own, for when both are needed. It's a visible cost, and `.copy()` on a `Copy` value is an error, since a plain use already copies it.

## Views

A function's parameters are views of the caller's values, unless they're `own` ([functions](functions.md#parameter-modes)). A view can be read and passed on, but not moved: the value belongs to the caller.

| Name | Is | Can be moved |
| --- | --- | --- |
| `x: T` | A read-only view of the caller's value | No |
| `x: mut T` | A view through which the caller's value changes | No |
| `x: own T` | The value itself, which the function now owns | Yes |
| `for x in xs` | A view of each element in turn | No |
| A name a pattern binds without taking its value apart | A view into the value matched | No |

```calx
fn count(words: List<String>) -> usize {
    return words.len()
}

fn with_extra(words: own List<String>, extra: String) -> List<String> {
    var all = words
    all.push(extra.copy())
    return all
}

fn main() {
    let ws: List<String> = ["a", "b"]
    println("{count(ws)}")                  // out: 2
    let more = with_extra(ws, String("c"))  // `ws` moves into `with_extra`
    println("{more}")                       // out: [a, b, c]
}
```

Views can't be stored or returned, so the compiler never needs lifetimes: it checks each function on its own.

## Moving fields

A field of a variable that owns its value can be moved out on its own. The variable's other fields can still be used, and the variable as a whole can't, until the field gets a value again:

```calx
struct Player {
    name: String
    title: String
    level: i32
}

fn main() {
    var p = Player { name: "Steve", title: "Builder", level: 3 }
    let name = p.name                        // moves the field out
    println("{name} {p.title} {p.level}")    // out: Steve Builder 3
    p.name = "Alex"                          // `p` is whole again
    let q = p
    println(q.name)                          // out: Alex
}
```

- A field of a view can't be moved, since the caller still owns it. Move a copy: `p.name.copy()`.
- An element of a list or array can't be moved out by indexing, since the list would be left with a hole. Take it out with `remove`, `swap_remove` or `pop` ([lists](lists.md)), or move a copy.
- A field of a new value, as in `make_player().name`, moves out, and the rest of the value is freed at once.

## Patterns

A `match` arm, `if let` or `let` with a pattern either looks at its value or takes it apart:

- It **takes the value apart** when one of the names it binds that owns memory is moved somewhere, has a field moved out of it, or is bound with `var`. Its names then own their parts, and the rest of the value is freed. The value counts as moved.
- Otherwise it **looks**: its names are views into the value. While they're in use, the value can't be changed or moved.

```calx
enum Msg {
    Chat(String)
    Kick { reason: String, ban: bool }
}

fn log(m: Msg) {
    match m {
        Chat(text) => println("chat: {text}"),
        Kick { reason, ban } => println("kick: {reason} {ban}"),
    }
}

fn reason_of(m: own Msg) -> String {
    return match m {
        Chat(text) => text,
        Kick { reason: r, .. } => r,
    }
}

fn main() {
    let m = Msg.Kick { reason: "spam", ban: true }
    log(m)                    // out: kick: spam true
    println(reason_of(m))     // out: spam
    log(Msg.Chat("hi"))       // out: chat: hi
}
```

A pattern can only take apart a value it owns. In `log`, `m` is a view, so moving `text` out would be an error.

## When values are freed

- At the end of the block that owns them, in reverse order of declaration.
- On `return`, `break` and `continue`, for everything the jump leaves.
- When a variable or field is assigned: the old value is freed once the new one is worked out, so the new value can read the old.
- At once, for a new value discarded with `_ = f()`.
- A new value that's only looked at, such as `make().len()` or a `String` passed to a view parameter, lives until the end of its statement. For an `if` or `while` condition, that's the end of the condition.

```calx
fn greeting(name: str) -> String {
    return "hello, {name}"
}

fn main() {
    println("{greeting("Ann").len()}")    // out: 10
    if greeting("Bo") == "hello, Bo" {    // freed before the block runs
        println("same")                   // out: same
    }
}
```

## Errors

Ownership is checked after everything else, so these errors appear once a program has no others:

- Using a value after it moved, or after it might have moved.
- Moving a value inside a loop that doesn't give it a new value before the next pass.
- Moving a view: a parameter that isn't `own`, a loop's element, or a name a pattern binds into a value it doesn't own.
- Moving an element out of a list or array, or a field out of a view.
- Using a variable whole while one of its fields is moved out.
- Changing or moving a value while a pattern's names are views into it, or while a `for` loop goes over it, what holds it or what's in it ([looping over a field](control-flow.md#looping-over-a-field)).

Each error says what to write instead: `own`, `.copy()`, `remove`, or the bound `Copy`.

## In C

Ownership costs nothing at run time:

- A move copies the value's bytes, and nothing frees the old place, since the compiler knows it's empty. There are no flags to say whether a value is still there.
- Each owning type gets a `static inline` C function that frees it, `cx_drop_T`, and one that copies it, `cx_copy_T`. The compiler calls them where values are freed and copied.
- A variable that might have moved by the end of its scope is zeroed where it moves, and freeing a zeroed value does nothing. So is a field that's moved out.
- A new value that's only looked at is a temporary declared before its statement, given its value inside it, and freed after it.

Every free gives the size of the block, which the compiler always knows: a list frees `cap` elements, a `String` `cap` bytes, and a table what its mask gives. So blocks have no header, and freeing a small one is a push onto a list. The allocator, in [runtime/calx_alloc.c](../../runtime/calx_alloc.c) (D95):

- Cuts blocks of up to 32 KiB in 40 sizes: steps of 16 bytes up to 128, then four for each doubling. Each thread keeps a list of free blocks of each size, and takes and gives them with no lock. A thread that frees more than it takes passes its blocks on, to the threads that run out.
- Gets bigger blocks from the system, and keeps one that's freed, up to 256 MiB in all, for the next block of its size. A table or list that grows and is freed every tick reuses the same memory, without the page faults that getting it from the system again would cost.

Debug builds use `malloc` instead, with each block's size in front of it. A block freed twice, or with a size it wasn't allocated with, stops the program with status 102, and so does a program that ends with blocks still allocated, reported on standard error. Since the compiler frees every value exactly once, each of these is a bug in Calx, and the test suite runs every test this way. Builds with AddressSanitizer use `malloc` too, so it sees every block.

Arenas, `Box<T>` for recursive types and `Shared<T>` for data many regions read are designed but not built yet ([spec](../CALX_SPEC.md#memory-model-d48)).
