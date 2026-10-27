---
title: Lists
description: List<T>, the growable array: literals, methods, indexing, loops, printing, and its C.
---

# Lists

`List<T>` is a growable array. It owns its elements, and frees them when it's freed ([ownership](ownership.md)).

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

`for x in xs` goes over the elements in order, and each `x` is a view of the element. The list can't be changed or moved during the loop, so it can't grow under the loop and leave it reading freed memory. To change the elements, loop over the indexes: `for i in 0usize..xs.len()`.

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

Whether a sort is stable, keeping equal elements in the order they were in, can't matter here: a derived `Order` compares everything a value holds, so equal elements are the same value. That leaves `sort` free to use whatever is fastest, which Rust's `sort` isn't, as a type there can order by part of itself. A type that writes its own `Order`, when that's built, will be sorted stably, as Java's `List.sort` is.

- **Lists already in order**, or in reverse, are found in one pass, which compares 16 elements at a time with no branch between.
- **Integers**, `char`, `rune` and `bool` in lists of a few hundred or more are radix sorted. A first pass finds which bytes differ between elements, and each of those takes one pass that moves every element by it, with no comparisons. When only one byte differs, as in a list of block IDs below 256, counting each value is enough, and the list is rewritten in place.
- **Everything else** is quicksorted, with the median of 3 elements as the pivot, or of 3 medians of 3 in longer runs. The partition never branches on a comparison, as a mispredicted branch would cost more than the comparison. A pivot equal to the one before it means the run holds many copies of it, and they're set aside in one pass, so lists of few distinct values sort in linear time. Runs of 16 or fewer are sorted by insertion, or for integers by a sorting network, and a run split badly too often is heapsorted, so the worst case stays O(n log n).
- **Structs and tuples of scalars** compare every field without a branch: `a.x < b.x | (a.x == b.x & a.y < b.y)`.

Moving an element copies its bytes, as every move does ([ownership](ownership.md)), so nothing is copied deeply or freed. Radix sorting takes a buffer the size of the list; nothing else allocates.

On the reference machine, an i9-13900K, against Rust 1.91's `sort` and `sort_unstable` in a release build for `x86-64-v3`, each pinned to one core, the best of 15 to 2,000 runs, in milliseconds:

| List | Calx `sort` | Rust `sort` | Rust `sort_unstable` |
| --- | --- | --- | --- |
| 1,000,000 random `i64` | 8.6 | 15.7 | 11.0 |
| 10,000 random `i64` | 0.066 | 0.075 | 0.053 |
| 1,000,000 `i64` of 16 values | 0.89 | 6.0 | 3.0 |
| 1,000,000 `i64` in order | 0.41 | 0.64 | 0.61 |
| 1,000,000 `i64` in reverse | 0.80 | 0.90 | 0.92 |
| 1,000,000 random `i32` | 3.9 | 13.1 | 10.6 |
| 200,000 structs of `i32`, `i32`, `i64` | 4.7 | 7.9 | 6.4 |
| 200,000 strings of 4 to 9 bytes | 7.7 | 15.0 | 13.0 |

## Printing

A list shows as `[1, 2, 3]`, the way Java's `ArrayList.toString` does, when its elements can be shown. Strings in a list appear without quotes, and nested lists print nested. Two lists compare with `==` when they have the same length and equal elements in order ([comparing and printing](comparing.md)).

## In C

- A `List<T>` is a struct of a pointer, a length and a capacity, named for its element type, such as `cx_list_i32`.
- It grows by doubling, to at least 4 elements. Java's `ArrayList` grows by half instead, so code that must match Java's capacity uses the `java.ArrayList` that's designed but not built yet ([spec](../CALX_SPEC.md#collections-d27)).
- The methods compile to inline C, so `push` is a length check and a store when there's room. Freeing a list frees each element, then the buffer.
- `xs.sort()` calls the runtime's sort for integers, `char`, `rune`, `bool`, and enums whose values go up as declared, which sort as their values. For any other element type, the file gets a sort of its own from the template `calx_sort.h`, with the type's comparison inlined.
