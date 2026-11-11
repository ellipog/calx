---
title: Slot maps
description: SlotMap<T> and Handle<T>, values reached by checked handles that go stale when the value is removed - making one, its methods, m[h] as a place, loops, generations, and its C.
---

# Slot maps

`SlotMap<T>` holds values, and gives back a `Handle<T>` for each one it takes in. The handle reaches that value until it's removed. After that the handle is stale: using it finds nothing, even when a new value has taken the old one's slot. A slot map owns its values, and frees them when it's freed ([ownership](ownership.md)).

It's how to refer to an entity, a player or a node from anywhere: a handle is two numbers, so it's `Copy`, it goes in any struct and through a queue to another thread, and it can't dangle.

```calx
struct Mob {
    name: String,
    hp: i32,
}

fn main() {
    var mobs = SlotMap<Mob>()
    let zombie = mobs.insert(Mob { name: String("zombie"), hp: 20 })
    let creeper = mobs.insert(Mob { name: String("creeper"), hp: 20 })
    mobs[zombie].hp -= 5
    println("{mobs[zombie].hp} {mobs.len()}")    // out: 15 2
    _ = mobs.remove(zombie)
    let skeleton = mobs.insert(Mob { name: String("skeleton"), hp: 20 })
    println("{mobs.contains(zombie)} {mobs.contains(skeleton)} {mobs[creeper].name}")    // out: false true creeper
}
```

The skeleton went into the zombie's slot, but `zombie` doesn't reach it.

## Making one

| Written | Gives |
| --- | --- |
| `SlotMap<T>()` | An empty slot map, which allocates nothing. `SlotMap()` works when the type is known from where it's used |
| `m.copy()` | A copy of the slot map and of each value, where every handle reaches the same value it does in `m` |

A handle comes only from `insert`. There's no literal for either type.

## Methods

| Method | Does |
| --- | --- |
| `m.insert(v)` | Puts `v` in and gives its handle, a `Handle<T>` |
| `m.remove(h)` | Takes out the value `h` reaches and gives it, or `none` if it was removed already. `h` goes stale |
| `m.contains(h)` | Whether `h` reaches a value |
| `m.get(h)` | A copy of the value `h` reaches, or `none`. The value type must be `Copy` |
| `if let v = m.find(h)` | A view of the value `h` reaches where it is, `mut` with `= mut m.find(h)`, found once. Only as what `if let`, `while let` or `let ... else` matches, as a [map's `find`](maps.md#methods) is |
| `m.len()`, `m.is_empty()` | The number of values, a `usize`, and whether it's 0 |
| `m.capacity()` | How many slots it has room for before it grows |
| `m.reserve(n)` | Makes room for `n` more values, so the next inserts don't allocate |
| `m.clear()` | Frees every value, and every handle goes stale |
| `m.handles()` | A list of the handles of its values, in slot order |

- Methods that change the slot map need a variable declared with `var`, or a `mut` parameter.
- `insert` moves the value in, and `remove` moves it out.
- A handle's methods are `h.index()` and `h.generation()`, its slot and that slot's generation, both `u32`.

## Indexing

`m[h]` is the value `h` reaches, as a place, so a part of it changes where it is:

```calx
struct Pos {
    x: i32,
    y: i32,
}

fn main() {
    var ps = SlotMap<Pos>()
    let p = ps.insert(Pos { x: 1, y: 2 })
    ps[p].x += 10
    ps[p] = Pos { x: ps[p].x, y: 0 }
    println("{ps[p].x} {ps[p].y}")    // out: 11 0
}
```

- A stale handle panics: "the slot map has no value for Handle(0, 1): it was removed, or the handle is another map's". Use `find`, `get` or `contains` where a handle may be stale, and `m[h]` where a stale one would be a bug.
- Assigning frees the old value.
- A value that owns memory can't be moved out by indexing, since that would leave a hole. Take it out with `remove`, or move a copy: `m[h].copy()`.
- A handle reaches values only in a slot map of its own type: `Handle<Pos>` indexes a `SlotMap<Pos>`, and a `Handle<Mob>` there is a type error.

## Loops

`for v in m` goes over the values in slot order, and each `v` is a view of the value. `for v in mut m` changes them where they are. The slot map can't be changed or moved during the loop.

```calx
fn main() {
    var hp = SlotMap<i32>()
    let a = hp.insert(10)
    _ = hp.insert(20)
    _ = hp.insert(30)
    _ = hp.remove(a)
    for v in mut hp {
        v -= 1
    }
    var total = 0
    for v in hp {
        total += v
    }
    println("{total} {hp}")    // out: 48 {Handle(1, 1): 19, Handle(2, 1): 29}
}
```

- `enumerate` counts the values the loop goes over, not slots.
- `rev`, `zip` and `step_by` don't go over a slot map.
- A [`parallel for`](parallel.md#over-slot-maps-maps-and-sets) goes over a slot map's values, and with `mut` each iteration changes its own value (CLX-71).
- To remove values while going over them, go over `m.handles()`, which is a list of its own: `for h in m.handles() { if m[h].hp <= 0 { _ = m.remove(h) } }`.

## Handles

A `Handle<T>` is a slot's index and the generation it had when its value went in.

- It's `Copy` and [`Send`](../std/thread.md), and one goes in a queue to another thread as two numbers.
- `==` and `<` compare the index, then the generation. A handle is a map's key or a set's value, since it has [`Hash`](maps.md#hash), but not [`JavaHash`](java-maps.md#javahash).
- It prints as `Handle(3, 1)`, the index and the generation.
- A handle from another slot map of the same type can reach a value there, if that map has a slot with that index and generation. Keep each map's handles with it.

## Generations

Each slot counts up a generation, and a handle reaches the slot's value only while the slot still has the generation the handle does.

- A slot's first generation is 1. Each insert into it and each removal from it adds one, so a slot with a value has an odd generation, and every handle has one.
- A removed value's slot goes on the front of a list of free slots, and the next insert takes the front one, whose memory was the last touched.
- `clear` keeps every slot and its generation, so no old handle reaches a value put in later.
- A slot whose generation would go past 2³² − 1 back round to 0 is retired instead, and never given out again, so no stale handle can ever reach a value. It takes four billion inserts and removals of one slot.
- A slot map holds at most 2³² − 1 values, since an index is 32 bits.

## Comparing and printing

- **`==` is true when the same handles reach equal values** in both. It needs values with `==`. A copy is equal to what it was copied from.
- **`{m}` prints a map of handles to values**, in slot order: `{Handle(0, 1): 1, Handle(1, 1): 20}`.
- A slot map has no `Hash` or `Order`, as a map has none.

## Rust's names

Rust has no slot map in its standard library; the `slotmap` crate is the usual one.

| Rust's `slotmap::SlotMap` | Calx |
| --- | --- |
| `SlotMap::new()`, `with_capacity(n)` | `SlotMap<T>()`, then `m.reserve(n)` |
| `insert(v)` | `m.insert(v)` |
| `remove(k)` | `m.remove(h)` |
| `get(k)`, `get_mut(k)` | `if let v = m.find(h)`, `if let v = mut m.find(h)`, or `m.get(h)` for a copy |
| `m[k]` | `m[h]` |
| `contains_key(k)` | `m.contains(h)` |
| `len()`, `is_empty()`, `capacity()`, `reserve(n)`, `clear()` | the same |
| `keys()` | `m.handles()` |
| `values()`, `values_mut()` | `for v in m`, `for v in mut m` |
| `clone()` | `m.copy()` |

- There's no `retain`, `drain`, `iter` of handles and values together, or secondary map yet.
- Rust's key is a type of its own, made with `new_key_type!`, or `DefaultKey` for any map; a Calx handle's type is its value's, `Handle<T>`.

## In C

- A `Handle<T>` is `cx_handle`, a struct of two `u32`s, the same for every `T`. It's passed and returned in one register. `==`, `<` and hashing read it as one 64-bit number, the index above the generation.
- A `SlotMap<T>` is a struct of a pointer to its slots, their count and capacity, the head of the free list and the number of values, named for its value type, such as `cx_slots_i64`. A slot is its generation, the next free slot's index, and the value.
- The functions come from the template `runtime/calx_slots.h`, included once for each slot map type in a file, and all inline: `m[h]` is a compare of the index with the length, a compare of the generations and a load; `insert` takes the free head, or a new slot at the end when there's room, as a list's `push` does.
- It grows by doubling, as a list does. A value never moves except when it grows, and a handle survives that, since it holds an index, not an address.
- A loop goes along the slots and passes over the free ones, whose generations are even: one test of a bit per slot.
- `copy()` copies the slots with their generations and free list, in one `memcpy` when the values are `Copy`.
