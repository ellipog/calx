---
title: fastutil's hash maps
description: OpenHashMap<K, V> and OpenHashSet<T>, which go over their keys in the order fastutil's open hash maps and sets do - which fastutil class each is, writing fastutil's constructors and methods, and their C.
---

# fastutil's hash maps

`OpenHashMap<K, V>` and `OpenHashSet<T>` are the open hash maps and sets of [fastutil](https://fastutil.di.unimi.it/), the library vanilla keeps most of its tables in: `Long2ObjectOpenHashMap`, `Object2IntOpenHashMap`, `LongOpenHashSet` and the rest. After the same operations, a loop goes over their keys in the order fastutil's would. They're for code where vanilla's order shows, such as the order it ticks chunks in or saves them. Everywhere else, [`Map` and `Set`](maps.md) are faster.

```calx
fn main() {
    var chunks = OpenHashMap<i64, String>()
    for x in 0i64..4 {
        chunks.insert((x << 32) | x, "chunk {x}")
    }
    println("{chunks}")    // out: {0: chunk 0, 4294967297: chunk 1, 12884901891: chunk 3, 8589934594: chunk 2}
    var counts = OpenHashMap<String, i32>()
    for w in ["stone", "dirt", "stone", "sand"] {
        if counts.contains(w) {
            counts[w] += 1
        } else {
            counts.insert(String(w), 1)
        }
    }
    println("{counts} {counts["stone"]}")    // out: {sand: 1, stone: 2, dirt: 1} 2
    var seen = OpenHashSet<i32>()
    for k in [5, 0, -3, 5] {
        seen.insert(k)
    }
    println("{seen} {seen.contains(0)}")    // out: {0, -3, 5} true
}
```

Both are built in, like `Map` and `Set`. Indexing, loops, `==`, printing, `copy()`, and moving and freeing work the same way as theirs ([maps and sets](maps.md)). The key type needs [`JavaHash`](java-maps.md#javahash).

## fastutil's classes

fastutil has a class for each key type and value type. The key type is what picks the order; the value type doesn't change it, so one Calx type stands for all of a key type's classes:

| Calx | fastutil |
| --- | --- |
| `OpenHashMap<i32, V>`, and `i8`, `i16`, `u8`, `u16`, `u32`, `char` and `rune` keys | `Int2ObjectOpenHashMap<V>`, `Int2IntOpenHashMap`, `Int2LongOpenHashMap`, and `Byte2…`, `Short2…` and `Char2…` |
| `OpenHashMap<i64, V>`, and `u64` and `usize` keys | `Long2ObjectOpenHashMap<V>`, `Long2IntOpenHashMap`, `Long2ByteOpenHashMap` and the rest |
| `OpenHashMap<K, V>` for any other `K` | `Object2ObjectOpenHashMap<K, V>`, `Object2IntOpenHashMap<K>`, `Object2BooleanOpenHashMap<K>` and the rest |
| `OpenHashSet<i32>`, `OpenHashSet<i64>`, `OpenHashSet<T>` | `IntOpenHashSet`, `LongOpenHashSet`, `ObjectOpenHashSet<T>`, and `ShortOpenHashSet` and `CharOpenHashSet` |

An integer key is placed by fastutil's `mix` of the number itself, as the primitive classes do. Any other key is placed by `mix` of its [`java_hash`](java-maps.md#javahash), which is its Java `hashCode`, as the `Object2…` classes do. So a `Long` key in an `Object2…` table, whose `hashCode` isn't the number, is written as a struct holding the `i64`, with the `java_hash` `Long.hashCode` gives.

## fastutil's methods

| fastutil | Calx |
| --- | --- |
| `new Long2ObjectOpenHashMap<>()`, `new LongOpenHashSet()` | `OpenHashMap<K, V>()`, `OpenHashSet<T>()` |
| `new …(expected)`, `new …(expected, f)` | `OpenHashMap<K, V>(n)`, `OpenHashMap<K, V>(n, f)`, with `f` an `f32` |
| `put(k, v)`, `add(x)` | `m.insert(k, v)`, `s.insert(x)` |
| `get(k)`, `getInt(k)`, `containsKey(k)`, `contains(x)` | `m[k]` or `m.get(k)`, `m.contains(k)`, `s.contains(x)` |
| `get(k)` after `defaultReturnValue(d)` | `m.get(k) ?? d` |
| `remove(k)`, `removeInt(k)` | `m.remove(k)`, which gives the value or `none` |
| `addTo(k, n)`, `putIfAbsent`, `computeIfAbsent`, `merge` | `if m.contains(k) { m[k] += n } else { m.insert(k, n) }`, and the like |
| `size()`, `isEmpty()`, `clear()`, `trim()` | `len()`, `is_empty()`, `clear()`, `trim()` |
| `putAll(m2)`, `addAll(c)` | `m.reserve(m2.len())`, then `m.insert` for each key of `m2`, in its order |
| `new …(m2)`, `new …(c)` | `OpenHashMap<K, V>(m2.len())`, `reserve(m2.len())`, then the inserts |
| `clone()` | `m.copy()` |
| Going over `keySet()`, `values()`, `long2ObjectEntrySet()` or `forEach` | `for (k, v) in m` |

- **Every way of adding a key puts it in the same slot**, the first empty one from where its hash points, so `addTo`, `putIfAbsent`, `computeIfAbsent` and `merge` are written as `contains` and `insert`.
- **`OpenHashMap(n, f)` sizes the table as fastutil does**: the power of two at least `n / f`, and at least 2. Without them it's fastutil's default, room for 16 keys at a load factor of 0.75, which is 32 slots. A load factor that isn't between 0 and 1 panics, as fastutil throws.
- **`reserve(n)` is the room `putAll` and `addAll` make**: room for `len() + n` keys, or, at a load factor of 0.5 or less, for `n` alone, as fastutil does.
- **`trim()` shrinks the table** to the smallest power of two that holds its keys, if that's smaller. **`clear` keeps the table's size.**
- **Removing a key can shrink the table**, to half its size, when it's left less than a quarter full and is bigger than it started and than 16 slots, as fastutil's does.
- **`copy()` is `clone()`**: the same slots, so the same order. A table built from another's keys, as fastutil's copy constructors build one, can go in another order, since the keys go in in the order the first table holds them:

```calx
fn main() {
    var ids = OpenHashSet<i32>(4, 0.5)
    for k in [17, 1, 33, 2] {
        ids.insert(k)
    }
    var built = OpenHashSet<i32>(ids.len())
    built.reserve(ids.len())
    for k in ids {
        built.insert(k)
    }
    println("{ids} {built} {ids.copy()}")    // out: {1, 17, 2, 33} {17, 1, 2, 33} {1, 17, 2, 33}
    var big = OpenHashSet<i64>(1000)
    for k in 1i64..7 {
        big.insert(k * 100)
    }
    println("{big}")    // out: {300, 600, 500, 400, 200, 100}
    big.trim()
    println("{big}")    // out: {300, 600, 200, 500, 100, 400}
}
```

## Order

The order is fastutil's, worked out from rules its tables keep:

- A table is a power of two slots. A key's slot is the first empty one from its hash, masked to the table's size, going up and wrapping around.
- An integer table marks an empty slot with 0, so the key 0 is kept apart, and a loop goes over it first. Then a loop goes from the last slot to the first.
- An insert that finds the table holding its maximum, the slots times the load factor and less than all of them, rehashes it to the size for one more key than it then holds. Rehashing goes from the last slot down, putting each key in the new table the same way.
- Removing a key moves each later key of its run that would be found from the gap back into it, as fastutil's `shiftKeys` does, so the table never needs a marker for a removed key.

`OpenHashMap` and `OpenHashSet` were checked against fastutil 8.5.18, the version vanilla 26.3 ships, by running the same random operations on both: inserts, removes, lookups and `addTo`, `putAll` and `addAll` as `reserve` and inserts, `trim`, `clear`, and new tables of random sizes and load factors from 0.25 to 0.99. The keys were `int`s, including 0 and dense runs of small numbers, `long`s, strings and a record whose many keys share each hash, in long phases of adding and of removing, so tables grew to 4,096 slots and shrank about 450 times in each run. 60 runs of 50,000 operations, 3 million in all, went over the same keys in the same order at every one of 375,060 checks.

## What's different

- A table can't change during a loop over it, which is a compile error. fastutil lets an iterator remove the key it's on, which moves other keys and changes the order the rest of the loop goes in; there's no way to write that yet, and code that does it waits for it.
- Keys and values can't be null. An `Object2…` table's null key, which fastutil keeps apart like an integer table's 0, isn't there.
- fastutil's `Reference2…` tables, and its `Custom` ones with a hash strategy, aren't built. A `Reference2…` table places keys by their addresses, so vanilla's own order changes from run to run, and where it shows, Calcite picks a fixed one (D27).
- fastutil's linked tables, `Long2ObjectLinkedOpenHashMap` and the rest, go in the order keys were added, and are still to build.

## In C

- The table is named for its types, such as `cx_omap_i64_String` and `cx_oset_i32`. It's an array of slots, each the key, or the key and value, in place; for keys that aren't integers, an array of words beside it; and the runtime's `cx_ohead`, the size, the keys and the load factor. A table that's made empty allocates nothing until its first key.
- An integer table's slots have room for one more, slot `n`, where the key 0 goes, and a 0 key in another slot means it's empty. A table of other keys keeps, for each slot, the 30 bits of the key's mixed hash that place it in any table, with the top bit set, and 0 for an empty slot, so finding a key compares words before keys, and rehashing never hashes a key again.
- A key and its value are in one slot, where fastutil keeps them in two arrays, so a lookup that finds a key reads one place.
- Finding, inserting, removing and rehashing compile to C specialized for the key and value types, as `Map`'s do. The sizes fastutil picks, which don't depend on the types, are the runtime's, in `calx_ohtable.c`, linked into a program only when it uses these tables.
