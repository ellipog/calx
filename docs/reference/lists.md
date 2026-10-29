---
title: Lists
description: List<T>, the growable array: literals, methods, indexing, loops, printing, and its C.
---

# Lists

`List<T>` is a growable array. It owns its elements, and frees them when it's freed ([ownership](ownership.md)). A queue, which takes from the front, is a [`Deque`](deques.md), and one that gives its least element first is a [`PriorityQueue`](priority-queues.md). A list of a tick's temporary values can take its memory from an [arena](arenas.md), which frees it with the others at once.

```calx
fn main() {
    var xs: List<i32> = [3, 1, 4]
    xs.push(1)
    xs.push(5)
    xs[0] = 2
    println("{xs} {xs.len()} {xs[4]}")    // out: [2, 1, 4, 1, 5] 5 5
    var sum = 0
    for x in xs {
        sum += x
    }
    println("{sum}")                      // out: 13
}
```

## Making one

| Written | Gives |
| --- | --- |
| `[a, b, c]` where a list is wanted | A list of those elements, in one allocation |
| `[]` where a list is wanted | An empty list, which allocates nothing |
| `[x; n]` where a list is wanted | `n` copies of `x`, which must be `Copy`. `n` can be worked out at run time |
| `List<T>()` | An empty list. `List()` works when the type is known from where it's used |
| `xs.copy()` | A copy of the list and of each element |

A literal is a list only where a list is wanted, such as `let xs: List<i32> = [1, 2]` or a `List` parameter. Elsewhere `[1, 2]` is an [array](types.md#arrays).

## Methods

| Method | Does |
| --- | --- |
| `xs.len()`, `xs.is_empty()` | The number of elements, a `usize`, and whether it's 0 |
| `xs.capacity()` | How many elements fit before it must grow |
| `xs.push(x)` | Adds `x` at the end |
| `xs.pop()` | Removes the last element and gives it, or `none` when the list is empty |
| `xs.insert(i, x)` | Puts `x` at index `i`, moving the rest up. `i` can be `len()`, which is the end |
| `xs.remove(i)` | Removes the element at `i` and gives it, moving the rest down |
| `xs.swap_remove(i)` | Removes the element at `i` and gives it, putting the last element in its place. Faster than `remove`, but changes the order |
| `xs.swap(i, j)` | Swaps two elements |
| `xs.truncate(n)` | Keeps the first `n` elements and frees the rest. Does nothing if there are `n` or fewer |
| `xs.reserve(n)` | Makes room for `n` more elements, so the next pushes don't allocate |
| `xs.clear()` | Frees every element, keeping the memory |
| `xs.contains(x)` | Whether an element `== x`. The element type must have `==` |
| `xs.sort()` | Puts the elements in order, from least to greatest. The element type must have [`Order`](comparing.md#order) ([sorting](#sorting)) |
| `xs.as_ptr()`, `xs.as_mut_ptr()`, `xs.set_len(n)` | A pointer to the elements, and setting the length, for [unsafe code](unsafe.md#pointers) |

- Methods that change the list need a variable declared with `var`, or a `mut` parameter.
- `push`, `insert` and the literals move the elements in. `pop`, `remove` and `swap_remove` move them out.

```calx
fn main() {
    var names: List<String> = ["Ann", "Bo"]
    names.insert(0, String("Cy"))
    names.push("Di")
    let first = names.remove(0)
    let last = names.pop() ?? "nobody"
    println("{first} {last} {names}")    // out: Cy Di [Ann, Bo]
    println("{names.contains("Bo")}")    // out: true
    names.truncate(1)
    println("{names}")                   // out: [Ann]
}
```

## Indexing

`xs[i]` is the element at index `i`, a `usize`. An index past the end panics, giving the index and the length.

- An element can be read, changed in place (`xs[i].count += 1`, `xs[i].push(x)`) and assigned. Assigning frees the old element.
- An element that owns memory can't be moved out by indexing, since that would leave a hole in the list. Take it out with `remove`, `swap_remove` or `pop`, or move a copy: `xs[i].copy()`.

```calx
fn main() {
    let xs: List<i32> = [1, 2, 3]
    let i = xs.len()
    println("{xs[i]}")    // panic: index 3 is out of bounds for length 3
}
```

`insert` past the end panics too, and `remove`, `swap_remove` and `swap` check their indexes the same way. `swap` works out both indexes before checking either.

## Loops

`for x in xs` goes over the elements in order, and each `x` is a view of the element. The list can't be changed or moved during the loop, so it can't grow under the loop and leave it reading freed memory. To change the elements, loop over the indexes: `for i in 0usize..xs.len()`. A loop over a list in a field leaves the other fields free to change ([looping over a field](control-flow.md#looping-over-a-field)).

## Sorting

`xs.sort()` sorts the list in place, by the elements' [`Order`](comparing.md#order). It takes O(n log n) time at worst, and linear time on a list that's already in order or in reverse.

```calx
struct Score {
    points: i32,
    name: String,
}

fn main() {
    var xs: List<i64> = [30, -5, 12, 0]
    xs.sort()
    println("{xs}")    // out: [-5, 0, 12, 30]
    var scores: List<Score> = [Score { points: 9, name: "b" }, Score { points: 3, name: "c" }, Score { points: 9, name: "a" }]
    scores.sort()
    println("{scores[0].name} {scores[1].name} {scores[2].name}")    // out: c a b
}
```

Whether a sort is stable, keeping equal elements in the order they were in, can't matter here: a derived `Order` compares everything a value holds, so equal elements are the same value. That leaves `sort` free to use whatever is fastest, which Rust's `sort` isn't, as a type there can order by part of itself. A type that [writes its own `Order`](comparing.md#writing-your-own), or holds one that does, is sorted stably, as Java's `List.sort` is ([below](#stable-sorting)).

- **Lists already in order** are found in one pass, which compares 16 elements at a time with no branch between. A list strictly in reverse is reversed in that same pass, from both ends at once, each block of 16 checked before it's swapped, so every element is read and written once. If the order breaks partway, the list is sorted as any other, the blocks already swapped no harder to sort than before.
- **Integers**, `char`, `rune` and `bool` in lists of a few hundred or more are radix sorted. A first pass finds which bits differ between elements. Values of 32 or 64 bits are then spread into buckets by the top of those bits, about one bucket for every element, so each bucket's elements come before the next one's, and an insertion pass that hardly moves anything finishes the list. A list of more than 65,536 is spread into buckets of about 4,096, each then sorted the same way while it's in cache. When few bytes differ in a long list, as in a million `i32`, a pass for each byte that differs is faster, moving every element by its byte with no comparisons. When only one byte differs, as in a list of block IDs below 256, counting each value is enough, and the list is rewritten in place.
- **Everything else** is quicksorted, with the median of 3 elements as the pivot, or of 3 medians of 3 in longer runs. The partition never branches on a comparison, as a mispredicted branch would cost more than the comparison. A pivot equal to the one before it means the run holds many copies of it, and they're set aside in one pass, so lists of few distinct values sort in linear time. Runs of 16 or fewer are sorted by insertion, and a run split badly too often is heapsorted, so the worst case stays O(n log n). Integers that would mostly fall in one bucket, as values spread over many powers of two do, are quicksorted too: a sample of 32 finds them before anything is counted.
- **Short runs of integers**, 32 or fewer, are sorted by a sorting network for 4, 8, 16 or 32 elements, the smallest that holds the run, with no branch on a comparison. A network wider than the run sorts some of its neighbors with it, which can't move them out of place: those before the run are no greater than anything in it, those after no less, and integers that are equal are the same value. A whole list of fewer than 32 integers is sorted by the network of its length, or of the next one up on a copy filled out with the type's greatest value.
- **Structs and tuples of scalars** compare every field without a branch: `a.x < b.x | (a.x == b.x & a.y < b.y)`.

Moving an element copies its bytes, as every move does ([ownership](ownership.md)), so nothing is copied deeply or freed. Radix sorting takes a buffer the size of the list, and so does a stable sort; nothing else allocates.

On the reference machine, an i9-13900K, against Rust 1.91's `sort` and `sort_unstable` in a release build for `x86-64-v3`, each pinned to one core, the best of 15 to 3,000 runs, in milliseconds:

| List | Calx `sort` | Rust `sort` | Rust `sort_unstable` |
| --- | --- | --- | --- |
| 1,000,000 random `i64` | 6.1 | 15.2 | 10.2 |
| 1,000,000 random `i64` below 2^53 | 6.3 | 15.3 | 10.2 |
| 100,000 random `i64` | 0.45 | 1.18 | 0.83 |
| 10,000 random `i64` | 0.039 | 0.097 | 0.068 |
| 1,000 random `i64` | 0.0026 | 0.0080 | 0.0053 |
| 10,000 lists of 16 random `i64` | 0.13 | 0.99 | 1.03 |
| 1,000,000 `i64` of 16 values | 0.97 | 4.7 | 2.4 |
| 1,000,000 `i64` powers of two, plus 0 to 6 | 4.9 | 6.9 | 4.4 |
| 1,000,000 `i64` in order | 0.38 | 0.46 | 0.47 |
| 1,000,000 `i64` in reverse | 0.39 | 0.82 | 0.78 |
| 1,000,000 random `i32` | 3.9 | 12.6 | 9.6 |
| 200,000 structs of `i32`, `i32`, `i64` | 4.8 | 7.6 | 5.9 |
| 200,000 strings of 4 to 9 bytes | 7.7 | 14.8 | 11.5 |

Each round sorts a list it hasn't sorted before, a copy just made, so neither side's branches can learn one. The powers of two are the one list where Rust's `sort_unstable` is faster, by about a tenth: their bits differ everywhere, but most of them fall in the spread's lowest bucket, so they're quicksorted, as Rust's are, after a pass that found which bits differ.

### Stable sorting

A type with a hand-written `Order` can call two different values equal, so its lists are sorted stably (D102):

- **Lists already in order**, or strictly in reverse, are found in one pass, as above. Reversing a list with no two elements equal keeps it stable.
- **Everything else** is quicksorted stably, with the same pivots, from more elements in long runs, and the same setting aside of a pivot's copies. The partition never branches on a comparison: each element is written either where the left side ends, never past the element being read, or to a buffer the size of the list, choosing by pointer, and the right side is copied back after the left.
- **Runs of 32 or fewer** are sorted with no branch on a comparison either: 4 at a time by a network that keeps equal elements in order, then merged in pairs, from both ends at once, so the front and back steps don't wait on each other.
- **A run split badly too often** is merge sorted, so the worst case stays O(n log n).

Merging from both ends with two runs of the same length can't read past either run unless `cmp` contradicts itself. A check at the end of each merge finds that, and `sort` panics rather than leave an element twice and another gone.

Against Rust's `sort_by` on a `key` field, its stable sort, in the same setup, for structs of an `i32` and an `i64` ordered by the `i64`:

| List | Calx `sort` | Rust `sort_by` |
| --- | --- | --- |
| 1,000,000 random | 22.1 | 27.0 |
| 200,000 random | 3.6 | 4.5 |
| 10,000 random | 0.098 | 0.124 |
| 1,000,000 of 16 values | 6.6 | 10.8 |
| 1,000,000 in order | 0.76 | 1.02 |
| 1,000,000 in reverse | 1.44 | 1.64 |

## Printing

A list shows as `[1, 2, 3]`, the way Java's `ArrayList.toString` does, when its elements can be shown. Strings in a list appear without quotes, and nested lists print nested. Two lists compare with `==` when they have the same length and equal elements in order ([comparing and printing](comparing.md)).

## In C

- A `List<T>` is a struct of a pointer, a length and a capacity, named for its element type, such as `cx_list_i32`.
- It grows by doubling, to at least 4 elements. Java's `ArrayList` grows by half instead, so code that must match Java's capacity uses the `java.ArrayList` that's designed but not built yet ([spec](../CALX_SPEC.md#collections-d27)).
- The methods compile to inline C, so `push` is a length check and a store when there's room. Freeing a list frees each element, then the buffer.
- Growing is a call to the runtime, out of line and marked cold, so a loop that pushes keeps its registers for itself. The call is given a copy of the capacity, not the list's own, so a list in a local keeps its pointer, length and capacity in registers, where no store to an element can change them. Given the capacity's address, the call would keep the list in memory, and since a byte, or an `i64` (the length's type), might be any of its fields as far as clang can tell, all three would be read back after every store (D158).
- A function with a loop that's given a list to change works on a copy of it in a local, which it writes back as it returns. Through the view, clang reads the fields back after every store, `restrict` or not, since as far as it can tell the list's buffer might be where the list itself is. Calx knows nothing else touches the list until the function returns: views can't be stored or returned, `mut` views never overlap (D48), and a panic ends the program. Writing back is a store for each field, which only a loop wins back, so a function without one uses the view. A struct that holds a list is copied a scalar at a time: the fields the function names, or all of it if it's given whole to a call or is a method's receiver that calls another, and only up to 8 scalars. Copied as one value, a part that's only ever copied, such as an optional string, would stay in memory and be copied again by each method clang puts inside the loop (D159).
- `xs.sort()` calls the runtime's sort for integers, `char`, `rune`, `bool`, and enums whose values go up as declared, which sort as their values. For any other element type, the file gets a sort of its own from the template `calx_sort.h`, with the type's comparison inlined.
- The runtime's sorts are `calx_sort.c`, compiled and linked only into programs that sort integers. A debug build leaves their networks out and sorts short runs by insertion: unoptimized, the networks would be several times the code and no faster. `tools/sort_networks.py` writes the networks, Batcher's odd-even merge sorts, and checks them on lists of zeros and ones, as a network that sorts every one of those sorts every list: all of them up to 16 elements, and 200,000 at random of 32 (D157).

A million values pushed onto a list, in ms, best of 50, from [`bench/push`](../../bench/push), on an i9-13900K pinned to one P-core:

| | Calx | Calx before D159 | Rust |
| --- | --- | --- | --- |
| 4 bytes each, onto a new list in a local | 0.92 | 0.92 | 2.52 |
| A byte each, onto a new list | 0.22 | 0.22 | 0.55 |
| An `i64` each, onto a new list | 0.84 | 0.84 | 2.45 |
| 4 bytes each, onto a list given to change | 0.79 | 1.70 | 1.71 |
| A byte each, onto a list given | 0.18 | 0.18 | 0.45 |
| An `i64` each, onto a list given | 0.31 | 0.30 | 0.64 |
| 4 bytes each, onto a list given, not inlined | 0.79 | 1.70 | 1.71 |
| A byte each, not inlined | 0.22 | 0.44 | 0.45 |
| An `i64` each, not inlined | 0.33 | 0.60 | 0.62 |
| A writer's methods, an int and a VarInt each, not inlined | 1.15 | 1.67 | 2.57 |

D158 took the first three from 1.01, 0.39 and 0.67. Rust's new lists lose most of their time to the Windows heap, which gives big blocks back to the system, so the next list's pages fault in again. Calx's allocator keeps them (D95). A list given to change is kept and cleared by the caller, which leaves the allocator out. Called directly, the byte and `i64` loops go inside the caller, where the list is a local, so D159 doesn't change them; the 4-byte loop clang keeps apart, now that it's also called through a function value. The writer is a struct of a list and an optional string, with a method for each kind of value, as Calcite's packets use.
