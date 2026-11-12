---
title: Maps and sets
description: "Map<K, V> and Set<T>, the native hash tables: methods, indexing, loops, their deterministic order, the Hash property, and their C."
---

# Maps and sets

`Map<K, V>` maps keys to values, and `Set<T>` holds values without repeats. Both are hash tables that own what they hold, and free it when they're freed ([ownership](ownership.md)). The key type needs [`Hash`](#hash).

```calx
fn main() {
    var counts = Map<String, i32>()
    for word in ["stone", "dirt", "stone"] {
        if let n = mut counts.find(word) {
            n += 1
        } else {
            counts.insert(String(word), 1)
        }
    }
    println("{counts["stone"]} {counts["dirt"]} {counts.len()}")    // out: 2 1 2
    var seen = Set<i32>()
    seen.insert(3)
    seen.insert(3)
    println("{seen.len()} {seen.contains(3)}")                      // out: 1 true
}
```

They're the native tables for code where the order of the keys can't be seen. Code ported from Java that must iterate in the order a `HashMap` does uses [`JavaHashMap` and `JavaHashSet`](java-maps.md), which go in Java's order, code that must go in the order of fastutil's tables, such as `Long2ObjectOpenHashMap`, uses [`OpenHashMap` and `OpenHashSet`](fastutil-maps.md), code that must go in the order keys went in, as Java's `LinkedHashMap` and fastutil's linked tables do, uses [`LinkedMap` and `LinkedSet`](linked-maps.md), and code that keeps its keys in order, as Java's `TreeMap` and fastutil's tree tables do, uses [`SortedMap` and `SortedSet`](sorted-maps.md).

## Making one

| Written | Gives |
| --- | --- |
| `Map<K, V>()`, `Set<T>()` | An empty table, which allocates nothing. `Map()` and `Set()` work when the type is known from where they're used |
| `m.copy()` | A copy of the table and of each key and value |

## Methods

| Method | Does |
| --- | --- |
| `m.len()`, `m.is_empty()` | The number of keys, a `usize`, and whether it's 0 |
| `m.capacity()` | How many keys fit before it must grow |
| `m.insert(k, v)` | Adds `k` with the value `v`. If `k` is there, `v` replaces its value, and the old value and the `k` given are freed |
| `m.remove(k)` | Removes `k` and gives its value, or `none` if it isn't there |
| `m.contains(k)` | Whether `k` is there |
| `m.get(k)` | A copy of `k`'s value, or `none`. The values must be `Copy`; others are looked at where they are with [`find`](#finding-a-value) |
| `m.find(k)` | `k`'s value where it is, or `none`, for `if let`, `while let` and `let ... else` to match ([finding a value](#finding-a-value)) |
| `m.clear()` | Frees every key and value, keeping the memory |
| `m.reserve(n)` | Makes room for `n` more keys, so the next inserts don't grow the table |
| `s.insert(x)`, `s.remove(x)` | Adds or removes `x` in a set. Adding a value that's there frees the one given |

A set has `len`, `is_empty`, `capacity`, `contains`, `clear` and `reserve` too.

- Methods that change the table need a variable declared with `var`, or a `mut` parameter.
- `insert` moves the key and the value in. `remove` moves the value out and frees the key.
- A `String` key can be looked up with a `str` or a `String`, and looking one up never allocates.

```calx
fn main() {
    var names = Map<i32, String>()
    names.insert(1, "Ann")
    names.insert(2, "Bo")
    names.insert(1, "Cy")
    println("{names[1]} {names.len()}")    // out: Cy 2
    names[2].push_str("b")
    println("{names[2]}")                  // out: Bob
    let gone = names.remove(1)
    println("{gone} {names.remove(1)} {names.len()}")    // out: some(Cy) none 1
    var health = Map<i32, f32>()
    health.insert(7, 20.0)
    println("{health.get(7)} {health.get(8)}")          // out: some(20.0) none
    names.clear()
    println("{names.is_empty()}")          // out: true
}
```

## Indexing

`m[k]` is the value of the key `k`. A key that isn't there panics, the way an index past a list's end does, so a key is added only by `insert`, never by assigning.

- A value can be read, changed in place (`m[k] += 1`, `m[k].push(x)`) and assigned. Assigning frees the old value.
- A value that owns memory can't be moved out by indexing, since that would leave a hole in the table. Take it out with `remove`, or move a copy: `m[k].copy()`.
- A set can't be indexed. `contains` says whether a value is there.

```calx
fn main() {
    var health = Map<i32, i32>()
    health.insert(1, 20)
    health[1] -= 5
    println("{health[1]}")    // out: 15
    health[2] = 20            // panic: the key isn't in the map; add it with `insert`
}
```

## Finding a value

`m.find(k)` looks `k` up once and gives its value where it is, or `none` when it isn't there. It's what `if let`, `while let` or `let ... else` matches, and the names it binds are views of the value, as with an optional that's a field. So `if m.contains(k) { use(m[k]) }`, which hashes `k` twice, is `if let v = m.find(k) { use(v) }`, and it works for values that own memory, which `get` can't copy.

- `if let v = mut m.find(k)` binds a `mut` view, which changes the value in the map. The map must be a `var` or a `mut` parameter. `let v = mut m.find(k) else { ... }` binds one until the block ends ([changing the parts in place](patterns.md#changing-the-parts-in-place)).
- While the names are in scope, the map can't change or move, so a view can't point at a value that's gone. With `mut`, the map is reached only through the names.
- A name can't be moved out, since the value stays in the map. Take the value out with `remove`, or move a copy with `.copy()`.
- `find` looks in a map that's a variable or a part of one, such as a field or a list's element.

```calx
fn main() {
    var tags = Map<String, List<String>>()
    tags.insert("stone", ["solid"])
    if let t = mut tags.find("stone") {
        t.push("grey")
    }
    if let t = tags.find("stone") {
        println("{t}")              // out: [solid, grey]
    }
    if let t = tags.find("air") {
        println("{t}")
    } else {
        println("no air")           // out: no air
    }
    count(mut tags, "stone")
    let t = tags.find("stone") else {
        return
    }
    println("{t.len()}")            // out: 3
}

fn count(tags: mut Map<String, List<String>>, k: str) {
    let t = mut tags.find(k) else {
        tags.insert(String(k), ["new"])
        return
    }
    t.push("seen")
}
```

Using it anywhere else is an error, since there'd be nothing to hold the map while the view is used:

```calx
fn main() {
    let m = Map<i32, String>()
    let v = m.find(1)    // error: `find` gives a view of the value where it is, so it's what `if let`, `while let` or `let ... else` matches: `if let v = m.find(key) { ... }`
    println("{v}")
}
```

## Loops

`for (k, v) in m` goes over a map's keys and values, and `for x in s` over a set's values. Each is a view. The table can't be changed or moved during the loop, so it can't grow under the loop and leave it reading freed memory. A loop over a table in a field leaves the other fields free to change ([looping over a field](control-flow.md#looping-over-a-field)).

`for v in m.values()` goes over a map's values alone, and `for v in mut m.values()` changes each one where it is. The keys can't change, since a change could move one, so `for (k, v) in mut m` is an error that says to write `m.values()`. Both work on every kind of map, and in a [`parallel for`](parallel.md#over-slot-maps-maps-and-sets).

```calx
fn main() {
    var stock = Map<String, i32>()
    stock.insert(String("apples"), 3)
    stock.insert(String("pears"), 5)
    for n in mut stock.values() {
        n += 10
    }
    var total = 0
    for n in stock.values() {
        total += n
    }
    println("{total} {stock["pears"]}")    // out: 28 15
}
```

```calx
fn main() {
    var stock = Map<String, i32>()
    stock.insert("stone", 64)
    stock.insert("dirt", 3)
    var total = 0
    for (_, n) in stock {
        total += n
    }
    println("{total}")    // out: 67
}
```

## Order

A loop goes over the keys in an order that depends only on the keys and on what was done to the table, in what order. There's no random seed, so it's the same in every run, on every platform and with any number of threads ([spec](../CALX_SPEC.md#native), D56). Code that leans on the order by mistake gives the same answer every time, even if it isn't the one Java would give.

The order isn't the order the keys went in, though, and any `insert` or `remove` can change it. Code where the order shows, such as what's sent to a client, uses a [`JavaHashMap`](java-maps.md), an [`OpenHashMap`](fastutil-maps.md), a [`LinkedMap`](linked-maps.md) or a [`SortedMap`](sorted-maps.md), whichever the original's table is, or sorts.

```calx
fn main() {
    var blocks = Set<String>()
    for name in ["stone", "dirt", "grass", "sand"] {
        blocks.insert(String(name))
    }
    println("{blocks}")    // out: {dirt, sand, stone, grass}
}
```

## `Hash`

A type can be a key when it has `Hash`, a derived property like `Equal` ([comparing and printing](comparing.md)): the compiler works it out, so there's nothing to write.

| Type | Has `Hash` |
| --- | --- |
| Integers, `bool`, `char`, `rune`, `str`, `String` | Yes |
| A struct, an enum, a tuple, an array, an optional or a `List` | When everything it holds has it |
| A float | No: `NaN` isn't equal to itself, so it could never be found |
| A pointer | No: a table's order would depend on addresses, which change from run to run |
| A map or a set | No |

A type with `Hash` has `Equal` too, and keys are compared with it. Generic code uses a table of `K` with the bound `K: Hash` ([generics](generics.md#bounds)):

```calx
fn count<K: Hash>(xs: List<K>) -> Map<K, i32> {
    var m = Map<K, i32>()
    for x in xs {
        if m.contains(x) {
            m[x] += 1
        } else {
            m.insert(x.copy(), 1)
        }
    }
    return m
}

fn main() {
    let words: List<String> = ["a", "b", "a"]
    let c = count(words)
    println("{c["a"]} {c["b"]}")    // out: 2 1
}
```

A key without `Hash` is an error where the table's type is written, with the reason:

```calx
struct Point {
    x: f64,
    y: f64,
}

fn main() {
    let seen = Set<Point>()    // error: `Point` can't be a key of a map or a set, since it has no `Hash`, since it holds `f64`, and floats have no `Hash`, since `NaN` isn't equal to itself
    println("{seen.len()}")
}
```

Like `Equal` and `Display`, `Hash` is a property: every type that can have it does. A struct or an enum can [write its own](comparing.md#writing-your-own), adding what its `eq` compares with `h.add(x)`. One that writes its own `Equal` has no derived `Hash`, since that would hash what `eq` ignores, so it writes `Hash` too to be a key.

## Comparing and printing

Two maps are equal when they have the same keys, with equal values; two sets when they have the same values. The order doesn't matter. A map shows as `{1: one, 2: two}`, and a set as `{1, 2}`, in the order a loop goes, when what they hold can be shown ([comparing and printing](comparing.md)).

## In C

- A table is a struct of a pointer to its control bytes, a mask, a length and how many more keys fit, 32 bytes, named for its types, such as `cx_map_i32_String` and `cx_set_i32`. Its slots and control bytes are one allocation, the slots ending where the control bytes start, so slot `i` is `CX_SLOT(S, ctrl, mask, i)` and the table needs no pointer to its slots. A loop of lookups works out where the slots start once, so finding a slot is indexing an array. An empty table has a mask of 0 and points at a shared group of empty bytes, and allocates nothing.
- It's a SwissTable, the layout of Rust's `hashbrown` and Abseil's `flat_hash_map`. Each slot has a control byte: empty, deleted, or 7 bits of the key's hash. A lookup starts at the slot the hash points to, compares 16 control bytes with the hash's 7 bits in one SSE2 instruction, and compares keys only where they match. An empty byte among the 16 means the key isn't there. On Arm64 it's one NEON compare, with three pairwise adds to make the mask, as NEON has no movemask. Machines with neither compare the 16 bytes in plain C. The layout and the order are the same on each.
- The table has a power-of-two number of slots, at least 4, and is at most 7/8 full. Removing a key leaves a deleted byte only when a lookup might have passed the slot; growing rebuilds without them.
- The hash is a folded multiply: the key, xored with a fixed seed, multiplied by a 64-bit constant into 128 bits, the halves xored together. Every bit of the result depends on every bit of the key, so keys that differ only in their top bits, like packed coordinates, spread over the table. Strings are hashed 8 bytes at a time, and structs and tuples part by part.
- `insert`, lookups and `remove` compile to inline C, specialized for the key and value types, so comparing two integer keys is one instruction and nothing is called through a pointer. Growing is a function of its own, since it's rare. Walking a table, to loop, free, copy, compare or print it, goes 16 control bytes at a time.
- `m.find(k)` is the lookup `m[k]` does, giving a pointer to the value, or a null one where `m[k]` would panic. The pattern matches what it points at, and its names point into the slot, so nothing is copied.
