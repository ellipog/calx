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

## Printing

A list of printable elements shows as `[1, 2, 3]`, the way Java's `ArrayList.toString` does. Strings in a list appear without quotes, and nested lists print nested.

## In C

- A `List<T>` is a struct of a pointer, a length and a capacity, named for its element type, such as `cx_list_i32`.
- It grows by doubling, to at least 4 elements. Java's `ArrayList` grows by half instead, so code that must match Java's capacity uses the `java.ArrayList` that's designed but not built yet ([spec](../CALX_SPEC.md#collections-d27)).
- The methods compile to inline C, so `push` is a length check and a store when there's room. Freeing a list frees each element, then the buffer.
