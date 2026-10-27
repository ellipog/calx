---
title: Java's hash maps
description: JavaHashMap<K, V> and JavaHashSet<T>, which go over their keys in the order Java's HashMap and HashSet do - JavaHash, writing Java's constructors and methods, and their C.
---

# Java's hash maps

`JavaHashMap<K, V>` and `JavaHashSet<T>` are Java's `HashMap` and `HashSet`. After the same operations, a loop goes over their keys in the order Java's would. They're for code where vanilla's order shows, such as the order it ticks things in or sends them to a client. Vanilla's fastutil tables are [`OpenHashMap` and `OpenHashSet`](fastutil-maps.md), Java's `LinkedHashMap` and `LinkedHashSet` are [`LinkedMap` and `LinkedSet`](linked-maps.md), and everywhere else, [`Map` and `Set`](maps.md) are faster.

```calx
fn main() {
    var blocks = JavaHashMap<String, i32>()
    for name in ["stone", "dirt", "grass", "sand", "gravel", "oak_log", "water", "lava"] {
        blocks.insert(String(name), i32.wrap(name.len()))
    }
    println("{blocks}")    // out: {gravel: 6, sand: 4, lava: 4, oak_log: 7, grass: 5, dirt: 4, water: 5, stone: 5}
    blocks["dirt"] += 10
    println("{blocks["dirt"]} {blocks.remove("sand")} {blocks.len()}")    // out: 14 some(4) 7
    var seen = JavaHashSet<i64>()
    for i in 0i64..6 {
        seen.insert((i << 32) | (i * 3))
    }
    println("{seen}")    // out: {0, 4294967299, 8589934598, 17179869196, 12884901897, 21474836495}
}
```

Both are built in, like `Map` and `Set`. Indexing, loops, `==`, printing, `copy()`, and moving and freeing work the same way as theirs ([maps and sets](maps.md)). The key type needs [`JavaHash`](#javahash).

## Java's methods

| Java | Calx |
| --- | --- |
| `new HashMap<>()`, `new HashSet<>()` | `JavaHashMap<K, V>()`, `JavaHashSet<T>()` |
| `new HashMap<>(n)`, `new HashSet<>(n)` | `JavaHashMap<K, V>(n)`, `JavaHashSet<T>(n)` |
| `put(k, v)`, `add(x)` | `m.insert(k, v)`, `s.insert(x)` |
| `computeIfAbsent(k, f)`, and `compute` and `merge` for a new key | `m.compute_if_absent(k, v)`: adds `k` with `v` if it isn't there, and says whether it did |
| `get(k)`, `containsKey(k)`, `contains(x)` | `m[k]` or `m.get(k)`, `m.contains(k)`, `s.contains(x)` |
| `remove(k)` | `m.remove(k)`, which gives the value or `none` |
| `size()`, `isEmpty()`, `clear()` | `len()`, `is_empty()`, `clear()` |
| `putAll(m2)` | `m.reserve(m2.len())`, then `m.insert` for each key of `m2`, in its order |
| `new HashMap<>(m2)`, `m2.clone()`, `HashMap.newHashMap(n)` | An empty map, `reserve(m2.len())` or `reserve(n)`, then the inserts |
| `new HashSet<>(c)` | An empty set, `reserve(max(c.len(), 12))`, then the inserts |
| Going over `keySet()`, `values()` or `entrySet()` | `for (k, v) in m` |

- **`insert` adds a new key at the end of its bin, and `compute_if_absent` at the front,** as Java's `put` and `computeIfAbsent` do, and they grow the table at different moments. Porting vanilla, each call becomes the one that adds keys the way the original does. `putIfAbsent` adds the way `put` does: `if not m.contains(k) { m.insert(k, v) }`.
- **`reserve(n)` is the room Java makes before `putAll` adds `n` keys.** A table that doesn't exist yet gets the bins for `n / 0.75`, rounded up, which is also what Java's copy constructors and `newHashMap` give it. One that exists doubles until its threshold reaches `n`.
- **`JavaHashMap(n)` asks for the first table**, of the power of two at least `n`. Without `n` it's 16 bins. Either way, the table is made with the first key.
- **`clear` keeps the table's size**, as Java's does, so keys added after it go where they would in Java.
- **`copy()` is the same table**, with the same bins and the same order. Java's `clone()` and `new HashMap<>(m)` build a table of their own, which can be smaller, so they're written as the table above says.
- `capacity` is only on `Map` and `Set`: Java's tables don't say how many keys fit.

```calx
fn main() {
    var put = JavaHashMap<i32, i32>()
    var computed = JavaHashMap<i32, i32>()
    for k in [0, 16, 32] {
        put.insert(k, 1)
        _ = computed.compute_if_absent(k, 1)
    }
    println("{put} {computed}")    // out: {0: 1, 16: 1, 32: 1} {32: 1, 16: 1, 0: 1}

    // 17 and 1 share a bin in a table of 16, but not in one of 32.
    var asked = JavaHashSet<i32>(32)
    var plain = JavaHashSet<i32>()
    for k in [17, 1, 33] {
        asked.insert(k)
        plain.insert(k)
    }
    println("{asked} {plain}")    // out: {1, 33, 17} {17, 1, 33}
}
```

## `JavaHash`

A key's bin comes from its `JavaHash`, which is Java's `hashCode`, and so does the slot of a key of an [`OpenHashMap`](fastutil-maps.md) that isn't an integer. `x.java_hash()` gives it, as an `i32`.

| Type | `java_hash` is |
| --- | --- |
| `i8`, `i16`, `i32`, and `u8`, `u16`, `u32` | The value, as Java's `Byte`, `Short` and `Integer` give it. An unsigned value's bits are the `int`'s |
| `i64`, `u64`, `usize` | `Long.hashCode`: the two halves xored |
| `bool` | `Boolean.hashCode`: 1231 or 1237 |
| `char`, `rune` | The value, as `Character` and `Integer` give it |
| `str`, `String` | `String.hashCode`, over the UTF-16 units ([strings](strings.md#javas-view-of-a-string)) |
| `List<T>` | `List.hashCode`: 1, then `31 * h` plus each element's |
| A struct or an enum | The one it writes, with `impl JavaHash` |
| `f32`, `f64` | `Float.hashCode` and `Double.hashCode`, for a `java_hash` that adds one up. Floats aren't `JavaHash`, though: `==` on them isn't Java's `equals`, so they can't be keys |

A Java class writes its own `hashCode`, and so does a struct or an enum here: there's no derived one, since Java's would be the class's own. The function is `fn java_hash(self) -> i32`, and like [`Hash`](comparing.md#writing-your-own) it needs `Equal`, which compares keys that share a hash:

```calx
struct BlockPos {
    x: i32,
    y: i32,
    z: i32,
}

// Vanilla's `Vec3i.hashCode`.
impl JavaHash for BlockPos {
    fn java_hash(self) -> i32 {
        return (self.y + self.z * 31) * 31 + self.x
    }
}

fn main() {
    var ps = JavaHashSet<BlockPos>()
    ps.insert(BlockPos { x: 31, y: 63, z: 1 })
    ps.insert(BlockPos { x: 0, y: 64, z: 0 })
    for p in ps {
        println("{p.x} {p.y} {p.z} {p.java_hash()}")    // out: 0 64 0 1984
    }                                                  // out: 31 63 1 2945
}
```

Generic code takes a key with the bound `K: JavaHash`, which brings `Equal` too ([generics](generics.md#bounds)). A key without one is an error where the table's type is written:

```calx
struct Point {
    x: f64,
    y: f64,
}

fn main() {
    let a = JavaHashSet<Point>()    // error: `Point` can't be a key of a `JavaHashMap` or a `JavaHashSet`, since it has no `JavaHash`, and a struct or an enum has the `JavaHash` it writes, as a Java class has the `hashCode` it writes: `impl JavaHash for Point`
    let b = JavaHashMap<f64, i32>()    // error: `f64` can't be a key of a `JavaHashMap` or a `JavaHashSet`, since it has no `JavaHash`, and floats have no `JavaHash`: `==` on them isn't Java's `equals`, so they can't be keys
    println("{a.len()} {b.len()}")
}
```

## Order

The order is Java's, worked out from rules Java's `HashMap` keeps:

- A key's bin is its `java_hash`, xored with itself shifted down 16, so the top bits count in a small table, masked to the number of bins: a power of two, 16 to start with.
- The table doubles when it holds more than three quarters of its bins. Each bin splits into the keys that stay and those that move up by the old size, each in the order they were in.
- A bin that `insert` makes longer than 8 keys, or `compute_if_absent` 8 long, becomes a red-black tree if the table has at least 64 bins; a smaller table doubles instead. A tree is ordered by hash, then, for keys with equal hashes, by `Order` where the key has it, which is Java's `compareTo` for signed numbers and for strings, by UTF-16 unit. Its root goes to the front of the bin, and a new key goes after its parent in the tree. A tree that doubling splits into 6 keys or fewer is a list again, and so is one that a remove leaves too shallow.

Where Java orders two keys by their addresses, which change from run to run, which happens when their hashes are equal and they can't be compared, these use a fixed order. That's one of Calcite's deliberate differences (D27): the same in every run, where vanilla's isn't.

`JavaHashMap` and `JavaHashSet` were checked against Java 25 by running the same random operations on both: inserts, `compute_if_absent`, removes, lookups, `reserve` and inserts as `putAll`, new tables with and without a capacity, and `clear`. The keys were numbers, `Long`s, strings and a struct of its own, many of them built to share bins, which made trees, split them and took them apart. 60 runs of 20,000 or 50,000 operations, about 1.9 million in all, went over the same keys in the same order at every one of 240,060 checks.

## What's different

- A table can't change during a loop over it, which is a compile error where Java throws `ConcurrentModificationException`. So there's no removing through an iterator, or `removeIf`.
- Keys and values can't be null. Code that stores null in a vanilla map writes an [optional](optionals.md) value.
- `==` compares the keys and values, and printing shows `{k: v}` as `Map` does, in Java's order.

## In C

- The table is the runtime's `cx_jtable` with its nodes' type, named for its types, such as `cx_jmap_String_i32` and `cx_jset_i32`. It has an array of bins, the nodes in one array, a length and Java's threshold; an empty table allocates nothing.
- A node is the key's spread hash, the index of the next node in its bin, and the key, or the key and value, in place. A bin holds the index of its first node, negative for a tree. A removed node waits for the next insert.
- A tree's links are in an array beside the nodes, made with the first tree, so a table without one pays nothing for them.
- Finding, inserting and removing compile to C specialized for the key and value types, as `Map`'s do, comparing a key only where the hash matches. Growing, and building, changing and splitting trees, don't depend on the key's type, so they're the runtime's, in `calx_jtable.c`, linked into a program only when it uses these tables. A tree orders its keys through a function the compiler writes for the key type.
