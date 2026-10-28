---
title: Arenas
description: Arena and ArenaList, lists that take memory from an arena and are freed all at once, the rules that keep them safe without lifetimes, parallel loops, and their C.
---

# Arenas

An `Arena` is memory that lists take from and that's freed all at once. A region keeps one, resets it at the start of each tick, and makes its temporary lists from it, so a tick's scratch lists cost a few instructions each and nothing to free (D114).

```calx
struct Pos {
    x: i32
    y: i32
}

struct Region {
    scratch: Arena
    ticks: i32
}

fn tick(r: mut Region, n: i32) -> i64 {
    r.scratch.reset()
    var todo = r.scratch.list<Pos>()
    for i in 0..n {
        todo.push(Pos { x: i, y: i * 2 })
    }
    var far = r.scratch.list<i64>()
    for p in todo {
        if p.y > 10 {
            far.push(i64(p.x) * 100 + i64(p.y))
        }
    }
    r.ticks += 1
    var sum = 0i64
    for v in far {
        sum += v
    }
    return sum
}

fn main() {
    var r = Region { scratch: Arena(), ticks: 0 }
    println("{tick(mut r, 8)} {tick(mut r, 4)} {r.ticks}")    // out: 1326 0 2
}
```

An arena list is a `List` in all but where its memory comes from: it has the same methods, indexing, loops, printing and `==`.

## Making one

| | |
| --- | --- |
| `Arena()` | An arena whose first chunk is 8 KiB |
| `Arena(n)` | An arena whose first chunk is `n` bytes |
| `a.list<T>()` | An empty `ArenaList<T>` that takes its memory from `a` |
| `a.list<T>(n)` | The same, with room for `n` elements |
| `a.reset()` | Frees everything taken from `a`, at once |
| `a.used()` | The bytes taken from `a` since it was made or last reset |

An arena gets its first chunk when a list first needs memory, so one that's never used costs only its small state. When a chunk runs out, the next is at least twice its size. `reset` frees them all and, when there was more than one, gets a single chunk of their total size, so after the first few ticks an arena's memory is one chunk that's reused tick after tick.

The type of an arena list can come from the variable instead of the call:

```calx
fn main() {
    let a = Arena()
    var xs: ArenaList<u16> = a.list(4)
    xs.push(3)
    xs.push(1)
    xs.sort()
    println("{xs} {xs.capacity() >= 4}")    // out: [1, 3] true
}
```

## Growing

A list's first buffer is 64 bytes, at least 4 elements, and each time it's full it doubles. When the list growing is the newest block in its arena's chunk, which it is unless another list took memory after it, it grows where it is, without copying. Otherwise it moves to new memory from the arena, and the old buffer stays unused until the next reset.

```calx
fn main() {
    let a = Arena(1024)
    var xs = a.list<u8>()
    for i in 0..100 {
        xs.push(u8.wrap(i))
    }
    println("{xs.capacity()} {a.used()}")    // out: 128 128
}
```

The 100 bytes took 128, not 64 and then 128 more.

## The rules

An arena list's memory is its arena's, and goes when the arena is reset. Calx has no lifetimes, so the checker keeps a list from outliving that memory with rules about where a list can be, which it checks in each function on its own (D114).

| | Allowed? |
| --- | --- |
| A local variable, `let xs = a.list<T>()` or `var` | Yes |
| A parameter, `xs: ArenaList<T>` or `xs: mut ArenaList<T>` | Yes |
| An `own` parameter, a field, a return type, an element, or inside an optional or a tuple | No |
| A generic function's argument | No: generic code could keep or copy it |
| `a.list<T>()` anywhere but as the whole value of a `let` | No |
| An arena that isn't a variable or a field, such as `Arena().list<i32>()` | No: nothing would keep the arena |
| Elements that own memory, such as `String` or `List<i32>` | No: a reset frees them without dropping them |
| Assigning to an arena list | No: empty it with `clear()` |
| `xs.copy()` | No: make a `List` with `xs.to_list()` |
| Changing or moving the arena, or what holds it, while a list made from it is in scope | No |
| Changing the other fields of what holds the arena | Yes |

The list holds its arena from its `let` until the end of its block, as a `for` loop holds what it goes over. So `reset` goes before the lists of a tick are made, or after the block they're in:

```calx
fn main() {
    var a = Arena()
    var total = 0
    for round in 0..3 {
        a.reset()
        var xs = a.list<i32>()
        for i in 0..round + 2 {
            xs.push(i)
        }
        for x in xs {
            total += x
        }
    }
    println("{total}")    // out: 10
}
```

What the checker says when a rule is broken:

```calx
struct Region {
    scratch: Arena
    n: i32
}

pub fn first(a: Arena) -> ArenaList<i32> {    // error: an `ArenaList` can only be the type of a variable or of a parameter that isn't `own`
    return a.list<i32>()    // error: an arena list is made in a `let` of its own
}

fn main() {
    var r = Region { scratch: Arena(), n: 0 }
    var xs = r.scratch.list<i32>()
    xs.push(1)
    r.n += 1
    r.scratch.reset()    // error: `r.scratch` can't be changed while `xs` holds memory from it
    let names = r.scratch.list<String>()    // error: an arena list holds only `Copy` elements
    let ys = xs.copy()    // error: an `ArenaList` can't be copied
    println("{names.len()} {ys.len()}")
}
```

## Parallel loops

Taking memory changes an arena, so the iterations of a `parallel for` can't share one ([parallel loops](parallel.md#what-an-iteration-can-touch)). Each takes from its own, reached with the loop variable:

```calx
fn main() {
    var arenas: List<Arena> = []
    for _ in 0..4 {
        arenas.push(Arena(256))
    }
    var sums: List<i64> = [0, 0, 0, 0]
    parallel for k in 0usize..4 {
        var ys = arenas[k].list<i64>()
        for i in 0..1000 {
            ys.push(i64(i) * i64.wrap(k + 1))
        }
        var s = 0i64
        for y in ys {
            s += y
        }
        sums[k] = s
    }
    println("{sums}")    // out: [499500, 999000, 1498500, 1998000]
}
```

A region that keeps its arena in itself gets this for free: `tick(mut regions[k])` reaches only that region's arena.

## Copies

`Arena` isn't `Copy`. A copy of an arena, as when a struct that holds one is copied, is a new, empty arena with the same first chunk size, since nothing in the old one belongs to the copy.

## In C

- An `Arena` is a pointer to its state on the heap: the free space in the newest chunk, from where the next block starts to where the chunk ends, the chunks, and the bytes in the full ones. A list keeps a pointer to the state, so the arena can live in a field of something that moves, such as an element of a growing `List<Region>`.
- An `ArenaList<T>` is a list's pointer, length and capacity, named for its element type, such as `cx_alist_i32`, and that pointer. Freeing one does nothing.
- Growing is inline C: with the element's size and alignment constants, it's a few instructions when the list is the newest block and its chunk has room, and only a new chunk is a call.
- In debug builds, `reset` fills the free chunk with `0xA5`, so a compiler bug that let something keep arena memory shows up as wrong values rather than old ones.

4,000 ticks of 64 regions, each making a list of up to 256 positions, a list of the ones it keeps, and 16 small lists, best of 3 runs, in ms:

| | ms |
| --- | --- |
| Calx `ArenaList`s, one arena reset each tick | 58 |
| Calx `List`s kept between ticks and cleared | 60 |
| Calx new `List`s each tick | 123 |
| Rust `Vec`s kept between ticks and cleared | 75 |
| Rust new `Vec`s each tick | 400 |

An arena makes the plain way of writing a tick, a new list for each thing, as fast as keeping and clearing buffers by hand. Rust's standard library has no arena; crates such as `bumpalo` weren't measured.
