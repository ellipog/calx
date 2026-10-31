---
title: Sorted maps and sets
description: SortedMap<K, V> and SortedSet<T>, which keep their keys in order, as Java's TreeMap and TreeSet and fastutil's tree tables do - their ends, the nearest key on either side, ranges, writing Java's and fastutil's methods, and their C.
---

# Sorted maps and sets

`SortedMap<K, V>` and `SortedSet<T>` keep their keys in their [`Order`](comparing.md#order), least first, as Java's `TreeMap` and `TreeSet` and fastutil's tree tables, such as `LongAVLTreeSet`, do. They find the nearest key on either side of any key, and go over the keys in a range:

```calx
fn main() {
    var layers = SortedMap<i32, String>()
    for (y, block) in [(64, "grass"), (-60, "bedrock"), (12, "stone"), (40, "dirt")] {
        layers.insert(y, String(block))
    }
    println("{layers}")    // out: {-60: bedrock, 12: stone, 40: dirt, 64: grass}
    println("{layers.floor(50)} {layers.ceiling(50)} {layers.lower(-60)}")    // out: some(40) some(64) none
    for (y, block) in layers.range(0..64) {
        println("{y} {block}")
    }
    // out: 12 stone
    // out: 40 dirt
    println("{layers.pop_first()} {layers.first()} {layers.last()}")    // out: some((-60, bedrock)) some(12) some(64)
}
```

Minecraft, for example, keeps the sections its entities are in as a `LongAVLTreeSet` of their packed positions, x first, then z, then y, so a chunk's sections are one range of keys, and it sorts resources and names with `TreeMap` and `TreeSet`.

Both are built in. Indexing, `len`, `is_empty`, `contains`, `get`, `remove`, `clear`, loops, printing, `copy()`, and moving and freeing work as [`Map`'s and `Set`'s](maps.md) do, and a loop goes in key order.

## Keys

The key type needs `Order`, as `<` and `sort` do, not `Hash`. Numbers, `bool`, `char`, `rune` and `String` have it, and structs, enums and tuples get it from what they hold ([comparing](comparing.md#order)). Floats don't, since `NaN` is neither less nor greater than anything.

- **Two keys are the same key when `Order` calls them equal**, as Java's `compareTo` does in a `TreeMap`. A key that's there keeps its place, and gets the new value.
- **`String`s go in byte order**, which is the order of their code points, and are found by a `str`: `names.contains("dirt")`.
- **There's no comparator.** Keys that go in another order are of a type with [an `Order` of its own](comparing.md#writing-your-own), as `new TreeMap<>(Comparator.reverseOrder())` becomes:

```calx
// The latest first.
struct Latest {
    tick: i64,
}

impl Order for Latest {
    fn cmp(self, other: Self) -> Ordering {
        return other.tick.cmp(self.tick)
    }
}

fn main() {
    var saves = SortedMap<Latest, String>()
    for t in [100i64, 300, 200] {
        saves.insert(Latest { tick: t }, "save at {t}")
    }
    for (at, name) in saves {
        println("{at.tick} {name}")
    }
    // out: 300 save at 300
    // out: 200 save at 200
    // out: 100 save at 100
}
```

## The ends and the nearest keys

- **`first()` and `last()` give the least key and the greatest**, as `K?`, `none` when the table is empty.
- **`pop_first()` and `pop_last()` take one out**: a map's give it with its value, as `(K, V)?`, and a set's as `T?`.
- **`floor(k)` and `ceiling(k)` give the nearest key that isn't after `k`, and the nearest that isn't before it**, `k` itself when it's there. **`lower(k)` and `higher(k)` give the nearest before it and after it**, never `k`.
- `first`, `last` and the four nearest keys copy the key out, so they need keys that are `Copy`. A `String` key is taken out with a pop, or read in a loop over a range from it.

## Ranges

`for (k, v) in m.range(a..b)` goes over the keys from `a` up to but not including `b`, in order, and `for x in s.range(a..b)` over a set's. It finds `a` as a lookup does, then goes along the keys from there, so it costs a lookup and the keys it goes over, whatever the table holds.

| Range | The keys |
| --- | --- |
| `a..b` | from `a`, up to but not including `b` |
| `a..=b` | from `a` to `b` |
| `a..` | from `a` to the greatest |
| `..b` | from the least, up to but not including `b` |
| `..=b` | from the least to `b` |

- **A range is only what a loop goes over.** `let r = m.range(a..b)` is an error, since there's no view of a table to keep.
- **The ends are looked up as keys are**, so a `String` table's are `str`s, and they're views: a variable that's an end is still there after the loop.
- **A range that ends before it starts panics**, as Java's `subMap` throws and Rust's `range` panics, since that's a mistake: `5..5` holds no keys, and `7..2` panics.
- The table can't change during the loop, as with any loop over it.

```calx
// A section's position packed into a key: x, then z, then y, each in two's complement.
fn section(x: i32, y: i32, z: i32) -> i64 {
    return ((i64(x) & 0x3FFFFF) << 42) | ((i64(z) & 0x3FFFFF) << 20) | (i64(y) & 0xFFFFF)
}

fn main() {
    var sections = SortedSet<i64>()
    for (x, y, z) in [(0, 4, 0), (0, -2, 0), (1, 4, 0), (0, 7, 0), (0, 4, 1)] {
        sections.insert(section(x, y, z))
    }
    // The sections of the chunk at 0, 0: from y 0 to y -1, which is all ones.
    var ys = List<i64>()
    for k in sections.range(section(0, 0, 0)..=section(0, -1, 0)) {
        ys.push((k << 44) >> 44)
    }
    println("{ys}")    // out: [4, 7, -2]
}
```

## Java's and fastutil's methods

| Java | fastutil | Calx |
| --- | --- | --- |
| `new TreeMap<>()`, `new TreeSet<>()` | `new Long2ObjectRBTreeMap<>()`, `new LongAVLTreeSet()` and the rest | `SortedMap<K, V>()`, `SortedSet<T>()` |
| `new TreeMap<>(comparator)` | `new …(comparator)` | a key type with that `Order` |
| `put(k, v)`, `add(x)` | `put(k, v)`, `add(x)` | `m.insert(k, v)`, `s.insert(x)` |
| `get(k)`, `containsKey(k)`, `contains(x)` | `get(k)`, `getInt(k)`, `containsKey(k)` | `m[k]`, `m.get(k)`, `m.contains(k)` |
| `remove(k)` | `remove(k)`, `removeInt(k)` | `m.remove(k)` |
| `firstKey()`, `lastKey()`, `first()`, `last()` | `firstLongKey()`, `lastIntKey()`, `firstLong()` and the rest | `first()`, `last()` |
| `pollFirstEntry()`, `pollLastEntry()`, `pollFirst()`, `pollLast()` | `firstLong()`, then `remove` | `pop_first()`, `pop_last()` |
| `floorKey(k)`, `ceilingKey(k)`, `lowerKey(k)`, `higherKey(k)`, and a set's `floor(x)` and the rest | `headSet(x)`, `tailSet(x)`, then `last()` or `first()` | `floor(k)`, `ceiling(k)`, `lower(k)`, `higher(k)` |
| `subMap(a, b)`, `subSet(a, b)` | `subMap(a, b)`, `subSet(a, b)` | `range(a..b)` |
| `subMap(a, true, b, true)` | | `range(a..=b)` |
| `tailMap(a)`, `tailSet(a)` | `tailMap(a)`, `tailSet(a)` | `range(a..)` |
| `headMap(b)`, `headSet(b)`, `headMap(b, true)` | `headMap(b)`, `headSet(b)` | `range(..b)`, `range(..=b)` |
| `new TreeMap<>(m2)` | `clone()`, `new …(m2)` | `m.copy()` |

- A range that starts after a key, as `tailMap(a, false)` does, starts at `higher(a)`.
- An empty table's `first()` is `none`, where Java's `firstKey()` throws, and its `floor(k)` is `none` where Java's `floorKey` gives `null`.

## Checked against Java

`SortedMap` and `SortedSet` were checked against Java 25's `TreeMap` and `TreeSet` by running the same random operations on both. The operations were inserts, removes, lookups, the four nearest keys, the ends, pops from either end, ranges of each kind as `subMap`, `headMap` and `tailMap`, changing values in place, copies with `==`, and `clear`. The keys were `i64`s from ranges of 30 to a billion, so the tables stayed small or grew to hundreds of thousands of keys, and `String`s from ranges of 50 and 3,000. 1.4 million operations in 6 runs gave the same answers at every one of 146 checks, in debug and release builds and under AddressSanitizer and UndefinedBehaviorSanitizer.

## What's different

- `String` keys go in code point order, and Java's `compareTo` compares UTF-16 units, which puts U+E000 to U+FFFF after the code points past U+FFFF. Tables without both kinds, such as any of ASCII names, go in the same order.
- A range is a loop, not a view: there's no `subMap` to change, or to keep.
- There's no loop from the greatest key back, as Java's `descendingMap` goes.
- A table can't change during a loop over it, which is a compile error. Java's iterators can remove the key they're on.
- Keys and values can't be null.

## In C

- The table is a B+ tree, named for its types, such as `cx_smap_i64_String` and `cx_sset_i32`: its root, its first and last leaves, how many keys it has, and how many levels of inner nodes are above its leaves. An empty one holds no memory.
- A leaf holds up to 128 keys, fewer for bigger ones, about 1 KB of keys and values, its keys in one array and its values in another, and is linked to the leaves on either side. An inner node holds up to 63 separators, the least key under each child but the first, and the children. A key that owns memory, such as a `String`, is copied into the separators, which can outlive it.
- A lookup goes down from the root, one node a level: 1 million keys are 3 levels of inner nodes and a leaf. A node of numbers is searched by counting the keys before the one looked for, which AVX2 does 4 at a time; others are searched by halves, without branches.
- A loop goes along the leaves, from one array to the next, and a range is a lookup and then the same walk.
- A full node splits in two, so keys put in at random fill about 70% of each node. A full last leaf that the next key would go at the end of starts a new leaf instead, as do the inner nodes above it, so keys put in in order fill every node. A node left with fewer than half takes a key from a neighbour, or is merged with it.

1 million `i64` keys to `i64` values, 1 to 4 million, the best of 3 rounds in one run, in ms; the range is 100,000 ranges of 400 keys wide:

| | in order | random | lookup | loop | range | floor | remove | pop first |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `SortedMap<i64, i64>` | 34 | 101 | 98 | 1.0 | 20 | 83 | 104 | 14 |
| Rust `BTreeMap<i64, i64>` | 51 | 126 | 140 | 6.8 | 69 | 137 | 138 | 18 |
| Java `TreeMap<Long, Long>` | 70 | 518 | 443 | 9.9 | 185 | 421 | 506 | 23 |
