---
title: Linked maps and sets
description: LinkedMap<K, V> and LinkedSet<T>, which go over their keys in the order they went in, as Java's LinkedHashMap and fastutil's linked tables do - their ends, moving keys to them, writing Java's and fastutil's methods, and their C.
---

# Linked maps and sets

`LinkedMap<K, V>` and `LinkedSet<T>` go over their keys in the order they went in. That's the order of Java's `LinkedHashMap` and `LinkedHashSet`, and of fastutil's linked tables, such as `Long2ObjectLinkedOpenHashMap`, which vanilla keeps its chunk holders and its region file cache in. A key can be moved to either end, and taken from either end.

```calx
fn main() {
    var blocks = LinkedMap<String, i32>()
    for w in ["stone", "dirt", "grass", "sand"] {
        blocks.insert(String(w), i32.wrap(w.len()))
    }
    blocks.insert(String("dirt"), 40)
    _ = blocks.remove("stone")
    blocks.insert(String("stone"), 50)
    println("{blocks}")    // out: {dirt: 40, grass: 5, sand: 4, stone: 50}
    var ids = LinkedSet<i64>()
    for k in [30i64, 10, 20] {
        ids.insert(k)
    }
    ids.insert_first(20)
    println("{ids} {ids.first()} {ids.last()}")    // out: {20, 30, 10} some(20) some(10)
    println("{ids.pop_last()} {ids}")    // out: some(10) {20, 30}
}
```

The order comes from what's done to the table, not from hashing, so one Calx type stands for Java's classes and for fastutil's, whatever the key type: `Long2ObjectLinkedOpenHashMap<V>`, `Object2IntLinkedOpenHashMap<K>` and `LongLinkedOpenHashSet` are all `LinkedMap` or `LinkedSet`. The key type needs [`Hash`](maps.md#hash), as a `Map`'s does.

Both are built in. Indexing, `len`, `contains`, `get`, `remove`, `clear`, `reserve`, `capacity`, loops, printing, `copy()`, and moving and freeing work as [`Map`'s and `Set`'s](maps.md) do.

## The order

- **`insert` puts a new key last.** A key that's there keeps its place, and gets the new value.
- **`remove` takes a key out**, and put back, it goes last.
- **`insert_first(k, v)` and `insert_last(k, v)` put a key at an end**, moving it there if it's there already, with the new value. A set's are `insert_first(x)` and `insert_last(x)`.
- **`move_to_first(k)` and `move_to_last(k)` move a key that's there to an end**, and give whether it was there.
- **`pop_first()` and `pop_last()` take out the key at an end**: a map's give it with its value, as `(K, V)?`, and a set's give it as `T?`, or `none` when the table is empty.
- **`first()` and `last()` give the key at an end**, as `K?`. They copy it out, so they need keys that are `Copy`; a `String` key is taken out with `pop_first`, or read in a loop, which starts at the first.
- **`==` compares the keys and values, not the order**, as Java's `equals` does. `copy()` keeps the order. `reserve` and growing don't change it.

## Java's and fastutil's methods

| Java | fastutil | Calx |
| --- | --- | --- |
| `new LinkedHashMap<>()`, `(n)`, `(n, f)` | `new Long2ObjectLinkedOpenHashMap<>()`, `(n)`, `(n, f)` | `LinkedMap<K, V>()`, and `reserve(n)` for the room |
| `put(k, v)`, `add(x)` | `put(k, v)`, `add(x)` | `m.insert(k, v)`, `s.insert(x)` |
| `putFirst(k, v)`, `putLast(k, v)`, `addFirst(x)`, `addLast(x)` | `putAndMoveToFirst(k, v)`, `putAndMoveToLast(k, v)`, `addAndMoveToFirst(x)`, `addAndMoveToLast(x)` | `insert_first`, `insert_last` |
| `get(k)`, `containsKey(k)` | `get(k)`, `getInt(k)`, `containsKey(k)` | `m[k]`, `m.get(k)`, `m.contains(k)` |
| | `getAndMoveToFirst(k)`, `getAndMoveToLast(k)` | `move_to_first(k)`, then `m[k]` if it gives `true` |
| `pollFirstEntry()`, `pollLastEntry()`, `removeFirst()`, `removeLast()` | `removeFirst()`, `removeLastInt()`, `removeFirstLong()` and the rest | `pop_first()`, `pop_last()` |
| `firstEntry().getKey()`, `lastEntry().getKey()`, `getFirst()`, `getLast()` | `firstLongKey()`, `lastKey()`, `first()`, `lastInt()` and the rest | `first()`, `last()` |
| `remove(k)` | `remove(k)`, `removeInt(k)` | `m.remove(k)` |
| `merge(k, n, Integer::sum)`, `putIfAbsent`, `computeIfAbsent` | `addTo(k, n)`, `putIfAbsent`, `computeIfAbsent` | `if m.contains(k) { m[k] += n } else { m.insert(k, n) }`, and the like |
| `putAll(m2)`, `addAll(c)` | the same | `reserve(m2.len())`, then `insert` for each key of `m2`, in its order |
| `new LinkedHashMap<>(m2)` | `clone()`, `new …(m2)` | `m.copy()` |
| `new LinkedHashMap<>(n, f, true)`, which moves a key last when it's read | | `move_to_last(k)` where it's read |
| `removeEldestEntry`, which a cache overrides | | `if m.len() == most { _ = m.pop_first() }` before an insert |

- A key that isn't there leaves the table as it is: fastutil's `getAndMoveToFirst` gives the default value, and `move_to_first` gives `false`.
- An empty table's `pop_first()` is `none`, where fastutil's `removeFirst` throws and Java's `pollFirstEntry` gives `null`.

Vanilla's region file cache keeps the files used last, as a fastutil table that `getAndMoveToFirst` reads, `putAndMoveToFirst` fills and `removeLast` empties when it's full:

```calx
fn main() {
    var cache = LinkedMap<i64, String>()
    for r in [1i64, 2, 3, 1, 4] {
        if cache.move_to_first(r) {
            continue
        }
        if cache.len() == 3 {
            let dropped = cache.pop_last()
            println("drop {dropped}")    // out: drop some((2, region 2))
        }
        cache.insert_first(r, "region {r}")
    }
    println("{cache}")    // out: {4: region 4, 1: region 1, 3: region 3}
}
```

## Checked against Java and fastutil

`LinkedMap` and `LinkedSet` were checked against Java 25's `LinkedHashMap` and `LinkedHashSet` and fastutil 8.5.18's linked tables at once, by running the same random operations on all three. The operations were inserts, `insert_first` and `insert_last` as `putFirst` and `putAndMoveToFirst`, removes, moves to either end as `getAndMoveToFirst`, taking keys from either end, reading the ends, lookups, `addTo`, `putAll`, copies, `clear` and new tables. The keys were `int`s, including a dense run of 48, `long`s, strings, and a record whose many keys share each hash, in long phases of adding and of taking out. Java and fastutil agreed with each other at every step, and Calx with both: 60 runs of 50,000 operations, 3 million in all, went over the same keys in the same order at every one of 375,060 checks.

## What's different

- A key needs `Hash`, not [`JavaHash`](java-maps.md#javahash), since the order doesn't come from hashing.
- A table can't change during a loop over it, which is a compile error. Java's and fastutil's iterators can remove the key they're on, and fastutil's can start from a key; there's no way to write those yet.
- There's no loop from the last key back, as Java's `reversed()` and fastutil's list iterators go.
- Access order isn't a kind of table, as Java's third constructor argument makes it: code that reads a key moves it with `move_to_last`.
- Keys and values can't be null.

## In C

- The table is a [`Map`'s](maps.md#in-c), named for its types, such as `cx_lmap_i64_String` and `cx_lset_i32`, with a list through its full slots after it: the runtime's `cx_lhead`, an array of two `cx_u32`s per slot for the slots before and after it, and the first and last slot. Each is the slot plus 1, and 0 for none, so a table that's made empty has an empty list.
- Finding a key is `Map`'s, so a lookup costs the same. Putting a key in also links it, and taking one out unlinks it, a few stores in the same cache line.
- Growing goes over the old slots in order, as `Map`'s does, and fixes the links up in a second pass, since following the list would miss the cache at every key.
- A loop follows the list. Its slots are in no order in memory, so over a table much bigger than the cache a loop waits on memory at each key, as fastutil's does, where Java's `LinkedHashMap` keeps its entries in memory in the order they were made. Up to 100,000 keys, which fit, it takes 1 to 7 ns a key.

1 million random `i64` keys to `i32` values, one run each, in ms:

| | insert | hit | move to first | loop | remove | pop last |
| --- | --- | --- | --- | --- | --- | --- |
| `LinkedMap<i64, i32>` | 30 | 13 | 24 | 69 | 31 | 53 |
| fastutil `Long2IntLinkedOpenHashMap` | 219 | 17 | 40 | 64 | 210 | 191 |
| Java `LinkedHashMap<Long, Integer>` | 97 | 25 | 67 | 6 | 38 | 90 |
