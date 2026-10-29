---
title: Deques
description: Deque<T>, a growable ring added to and taken from at both ends, which is Java's ArrayDeque and Rust's VecDeque - its methods, indexing, loops, Java's and Rust's names, and its C.
---

# Deques

`Deque<T>` is a growable ring of elements, added to and taken from at both ends. It's Java's `ArrayDeque`, the queues vanilla keeps as `Queue` or `Deque`, and fastutil's `LongArrayFIFOQueue`. It owns its elements, and frees them when it's freed ([ownership](ownership.md)).

```calx
fn main() {
    var d = Deque<i32>()
    d.push_last(2)
    d.push_last(3)
    d.push_first(1)
    println("{d} {d.len()} {d[0]} {d.last()}")    // out: [1, 2, 3] 3 1 some(3)
    let front = d.pop_first()
    let back = d.pop_last()
    println("{front} {back} {d}")    // out: some(1) some(3) [2]
}
```

## Making one

| Written | Gives |
| --- | --- |
| `Deque<T>()` | An empty deque, which allocates nothing. `Deque()` works when the type is known from where it's used |
| `[a, b, c]` where a deque is wanted | A deque of those elements, in one allocation |
| `[x; n]` where a deque is wanted | `n` copies of `x`, which must be `Copy` |
| `d.copy()` | A copy of the deque and of each element |

## Methods

| Method | Does |
| --- | --- |
| `d.len()`, `d.is_empty()` | The number of elements, a `usize`, and whether it's 0 |
| `d.capacity()` | How many elements fit before it must grow |
| `d.push_last(x)`, `d.push_first(x)` | Adds `x` at the end, or before the first element |
| `d.pop_first()`, `d.pop_last()` | Removes the first or the last element and gives it, or `none` when the deque is empty |
| `d.first()`, `d.last()` | A copy of the first or the last element, or `none`. The element type must be `Copy`; read any other in place with `d[0]` and `d[d.len() - 1]` |
| `d.reserve(n)` | Makes room for `n` more elements, so the next pushes don't allocate |
| `d.clear()` | Frees every element, keeping the memory |
| `d.contains(x)` | Whether an element `== x`. The element type must have `==` |

- Methods that change the deque need a variable declared with `var`, or a `mut` parameter.
- `push_first`, `push_last` and the literals move the elements in. `pop_first` and `pop_last` move them out.

A flood fill takes from the front of its queue and adds at the back, so it reaches what's nearer first, as vanilla's light and fluid updates do:

```calx
fn main() {
    let w = 5usize
    let walls: List<bool> = [false, false, true, false, false,
                             false, true, true, false, false,
                             false, false, false, false, true,
                             true, true, false, true, false,
                             false, false, false, false, false]
    var dist: List<i32> = [-1; 25]
    var queue = Deque<usize>()
    dist[0] = 0
    queue.push_last(0)
    while let at = queue.pop_first() {
        var near = List<usize>()
        if at % w > 0 {
            near.push(at - 1)
        }
        if at % w + 1 < w {
            near.push(at + 1)
        }
        if at >= w {
            near.push(at - w)
        }
        if at + w < walls.len() {
            near.push(at + w)
        }
        for next in near {
            if not walls[next] and dist[next] < 0 {
                dist[next] = dist[at] + 1
                queue.push_last(next)
            }
        }
    }
    println("{dist[4]} {dist[19]} {dist[23]}")    // out: 8 9 7
}
```

## Indexing

`d[i]` is the element `i` places after the first, a `usize`, wherever that is in the ring. An index past the end panics, giving the index and the length, as a list's does.

- An element can be read, changed in place (`d[i].count += 1`) and assigned. Assigning frees the old element.
- An element that owns memory can't be moved out by indexing, since that would leave a hole. Take it out with `pop_first` or `pop_last`, or move a copy: `d[i].copy()`.

## Loops

`for x in d` goes over the elements from the first to the last, and each `x` is a view of the element. The deque can't be changed or moved during the loop.

## Java's and Rust's names

| Java's `ArrayDeque` | Rust's `VecDeque` | Calx |
| --- | --- | --- |
| `addLast(x)`, `add(x)`, `offer(x)`, `offerLast(x)` | `push_back(x)` | `d.push_last(x)` |
| `addFirst(x)`, `offerFirst(x)`, `push(x)` | `push_front(x)` | `d.push_first(x)` |
| `pollFirst()`, `poll()`, `removeFirst()`, `remove()`, `pop()` | `pop_front()` | `d.pop_first()` |
| `pollLast()`, `removeLast()` | `pop_back()` | `d.pop_last()` |
| `peekFirst()`, `peek()`, `getFirst()`, `element()` | `front()` | `d.first()`, or `d[0]` |
| `peekLast()`, `getLast()` | `back()` | `d.last()`, or `d[d.len() - 1]` |
| `size()`, `isEmpty()` | `len()`, `is_empty()` | `d.len()`, `d.is_empty()` |
| `contains(x)`, `clear()` | `contains(&x)`, `clear()` | `d.contains(x)`, `d.clear()` |
| `new ArrayDeque<>(c)`, `addAll(c)` | `extend(c)` | `push_last` for each element of `c`, in its order |
| `clone()` | `clone()` | `d.copy()` |
| | `d[i]`, `get(i)` | `d[i]` |

- Java's `push` and `pop` are at the front, as a stack's top: they're `push_first` and `pop_first`. A `List` is the faster stack, with `push` and `pop` at its end.
- Calling a Java or Rust name on a deque is an error that gives the Calx one, as in "`Deque<i32>` has no method `poll`; a deque's is `pop_first`".
- `removeFirst`, `pop` and `element` throw on an empty deque, where `pop_first` and `first` give `none`.

## Checked against Java

`Deque` was checked against Java 25's `ArrayDeque` by running the same 800,000 random operations on both: pushes and pops at each end, reads and writes by index, `contains`, `first` and `last`, loops, copies compared with `==`, and `clear`, in runs that keep the deque short and runs that grow it to thousands of elements, growing while the ring has wrapped. Both printed the same length and the same hash of every result at each of 84 checkpoints. It runs clean under AddressSanitizer and UBSan.

## What's different

- **`==` compares the elements**, in order from the first, as a list's does. Java's `ArrayDeque` has no `equals`, so two Java deques are equal only if they're the same object.
- **A deque is a map's key or a set's value** when its elements have [`Hash`](maps.md#hash). It hasn't [`JavaHash`](java-maps.md#javahash), since Java's `ArrayDeque` has no `hashCode` of its own.
- **A deque can't be ordered** with `<` or sorted, as Java's can't. A list can.
- There's no loop from the last element back, as Java's `descendingIterator` goes, and no way yet to remove from the middle, as Java's `remove(x)`, `removeIf` and iterators do.
- Elements can't be null.

## In C

- A `Deque<T>` is a struct of a pointer, a length, a capacity and the index of the first element, named for its element type, such as `cx_deque_i64`. The first three are a [list's](lists.md#in-c), so a literal is built the same way.
- The element `k` after the first is at `head + k`, less the capacity when that's past the end: an add and a compare, never a division. The capacity needn't be a power of two, so a literal's buffer is exactly its length.
- It grows by doubling, to at least 4 elements, as a list does. When the ring has wrapped, the shorter of its two runs moves, so the elements stay in one ring. Growing is a cold call given copies of the head and the capacity, as a list's is given a copy of its capacity, so a deque in a local keeps its fields in registers ([lists](lists.md#in-c), D158). A function with a loop that's given a deque to change works on a copy of it, as one given a list does (D159).
- The methods compile to inline C, so `push_last` is a length check, an add, a compare and a store when there's room.
- A loop goes over at most two runs, from the first element to the end of the buffer, then from its start, each a plain loop that clang can unroll and vectorize.
- `copy()` gives a deque whose ring starts at 0, with exactly room for its elements.

10 million `i64`s, one run each, in ms:

| | push last | pop first | a queue of 1,000, one in and one out | 5M at each end | loop | random reads |
| --- | --- | --- | --- | --- | --- | --- |
| `Deque<i64>` | 21 | 7 | 6 | 27 | 4 | 101 |
| Rust `VecDeque<i64>` | 38 | 9 | 14 | 45 | 9 | 130 |
| Java `ArrayDeque<Long>` | 83 | 20 | 121 | 163 | 18 | 274 |

Java's `ArrayDeque` has no index, so its random reads are on an `ArrayList` copy.
