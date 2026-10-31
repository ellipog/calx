---
title: Priority queues
description: PriorityQueue<T>, least first, with its elements in the places Java's PriorityQueue puts them, so a loop goes in the order Java's iterator does - its methods, taking out marked elements as a Java iterator does, Java's names, and its C.
---

# Priority queues

`PriorityQueue<T>` gives its least element first, as Java's `PriorityQueue` does. It's a binary heap in an array, and every change moves the elements as Java's does, so after the same changes the array holds the same elements in the same places. A loop and printing go along the array, which is the order Java's iterator and `toString` go in:

```calx
fn main() {
    var q = PriorityQueue<i32>()
    for x in [5, 3, 8, 1, 9, 2, 7] {
        q.push(x)
    }
    println("{q.peek()} {q.len()}")    // out: some(1) 7
    println("{q}")    // out: [1, 3, 2, 5, 9, 8, 7]
    var order = List<i32>()
    while let x = q.pop() {
        order.push(x)
    }
    println("{order}")    // out: [1, 2, 3, 5, 7, 8, 9]
}
```

Minecraft, for example, keeps each chunk's scheduled block and fluid ticks in a `PriorityQueue`, drained by time, then priority, then the order they were scheduled in, and the chunks with ticks due in another. It takes ticks out with a loop over the queue that removes with its iterator, whose order is the array's, so the ticks that are left are where Java's would be.

It's built in. `len`, `is_empty`, `contains`, `clear`, `reserve`, `capacity`, loops, printing, `copy()`, and moving and freeing work as [`List`'s](lists.md) do.

## The order

The element type needs `Order`, as `<` and `sort` do. Numbers, `bool`, `char`, `rune` and `String` have it, and structs, enums and tuples get it from what they hold ([comparing](comparing.md#order)). Floats don't, since `NaN` is neither less nor greater than anything.

- **Elements that `Order` calls equal come out in no set order**, as in Java. Give each element a last field that no two share, such as the order it was added in, and no two are equal.
- **There's no comparator.** Elements that go in another order are of a type with [an `Order` of its own](comparing.md#writing-your-own), as `new PriorityQueue<>(comparator)` becomes:

```calx
// Minecraft's drain order for scheduled ticks: the time, then the priority, then the order they
// were scheduled in.
struct Tick {
    at: i64,
    priority: i32,
    order: i64,
    x: i32,
}

impl Order for Tick {
    fn cmp(self, other: Self) -> Ordering {
        if self.at != other.at {
            return self.at.cmp(other.at)
        }
        if self.priority != other.priority {
            return self.priority.cmp(other.priority)
        }
        return self.order.cmp(other.order)
    }
}

fn main() {
    var ticks = PriorityQueue<Tick>()
    var order = 0i64
    for (at, priority, x) in [(20i64, 0, 1), (10i64, 1, 2), (10i64, -1, 3), (20i64, 0, 4)] {
        ticks.push(Tick { at: at, priority: priority, order: order, x: x })
        order += 1
    }
    while let t = ticks.pop() {
        println("{t.at} {t.priority} {t.x}")
    }
    // out: 10 -1 3
    // out: 10 1 2
    // out: 20 0 1
    // out: 20 0 4
}
```

## Methods

- **`push(x)` puts an element in.**
- **`pop()` takes the least one out**, as `T?`, `none` when the queue is empty, and **`peek()` gives it** without taking it out. `peek` copies it out, so it needs elements that are `Copy`; a `String` is taken out with `pop`, or read in a loop, which starts with it.
- **`remove(x)` takes out the first element equal to `x` along the array**, and says whether there was one, as Java's `remove(o)` does. It goes along the array, as `contains` does, so it takes time in proportion to the queue's length.
- **`remove_marked(marks)` takes out the elements whose marks are set** (below).
- A queue can't be indexed, since only the least element's place means anything, and isn't written as a list: it's made with `PriorityQueue<T>()`, and each element pushed.

## Taking out marked elements

`q.remove_marked(marks)` takes out each element whose mark is set, `marks` being a `List<bool>` with one mark for each element, in the order a loop over the queue goes. It moves the elements as a Java loop over the queue that calls its iterator's `remove` on those elements does, so the ones left are in Java's places. Java's `removeIf` becomes a loop that marks the elements, then `remove_marked`:

```calx
struct Tick {
    at: i64,
    order: i64,
    x: i32,
}

fn main() {
    var ticks = PriorityQueue<Tick>()
    var order = 0i64
    for (at, x) in [(30i64, 1), (10i64, 5), (20i64, 2), (10i64, 7), (40i64, 5), (20i64, 9)] {
        ticks.push(Tick { at: at, order: order, x: x })
        order += 1
    }
    // The ticks at x 5, as Java's `removeIf(tick -> tick.pos().getX() == 5)`.
    var marks = List<bool>()
    for t in ticks {
        marks.push(t.x == 5)
    }
    ticks.remove_marked(marks)
    var left = List<String>()
    for t in ticks {
        left.push("{t.at}/{t.x}")
    }
    println("{left}")    // out: [10/7, 20/9, 20/2, 30/1]
}
```

- A mark is read for the element that was in that place when the loop marked it, wherever the removing moves the elements.
- **A list with a mark for each element panics otherwise**, since that's a mistake: the queue changed after the loop, or the loop missed some.
- The removing doesn't call back into Calx, so it can't change the queue as it goes.

## Java's names

| Java | Calx |
| --- | --- |
| `new PriorityQueue<>()` | `PriorityQueue<T>()` |
| `new PriorityQueue<>(comparator)` | an element type with that `Order` |
| `add(x)`, `offer(x)` | `push(x)` |
| `poll()` | `pop()` |
| `peek()`, `element()` | `peek()` |
| `remove(o)` | `remove(x)` |
| `contains(o)` | `contains(x)` |
| `size()`, `isEmpty()` | `len()`, `is_empty()` |
| `clear()` | `clear()` |
| `removeIf(test)`, an iterator's `remove()` | a loop that marks, then `remove_marked(marks)` |
| `new PriorityQueue<>(q2)` | `q.copy()` |
| `for (T x : q)`, `toString()` | `for x in q`, `"{q}"` |

- An empty queue's `pop()` and `peek()` are `none`, where Java's `poll()` and `peek()` give `null`, and `element()` and `remove()` throw.
- Using Java's name for a method, as `q.add(x)`, is an error that gives Calx's.

## Checked against Java

`PriorityQueue` was checked against Java 25's `PriorityQueue` by running the same random operations on both and comparing the array, in loop order, at every checkpoint. The operations were pushes, pops, peeks, `contains`, `remove`, iterator removal, copies and `clear`. The elements were structs ordered by one field of a few values, so most compared equal, which tests where ties go; plain `i64`s; and structs and tuples ordered member by member, of signed and unsigned members of 8 to 64 bits and `bool`s. 3.4 million operations in 16 runs, and 5 more runs that filled queues of up to 400,000 elements and emptied them, gave the same arrays at every one of 710 checks, in debug and release builds and under AddressSanitizer and UndefinedBehaviorSanitizer.

## What's different

- **The least comes first**, as in Java. Rust's `BinaryHeap` gives the greatest first.
- **`==` compares the elements in the order of the arrays**, so two queues of the same elements that were put in in different orders can be unequal. Java's `equals` on a `PriorityQueue` is identity.
- A queue has no `Hash` and no `Order`, as Java's has no `hashCode` or `compareTo` of its own, so it can't be a key of a map or a set.
- A queue can't change during a loop over it, which is a compile error. Java's iterator can remove the element it's on, which is `remove_marked`.
- Elements can't be null.

## In C

- The queue is a list's struct, named for its type, such as `cx_heap_i32`: a pointer to the array, its length and its capacity. It grows as a list does. An empty one holds no memory.
- The children of place `k` are at `2k + 1` and `2k + 2`, and no element comes before its parent, as in Java.
- `push` puts the element at the end and moves it up past each parent it comes before.
- `pop` takes the first, and puts the last in its place, where Java's moves it down, a level at a time, until neither child comes before it. Calx's moves the gap down to the bottom along the children Java's would pick, then moves the last element back up past the ones it doesn't come after. The children it picks never get less, so it stops where Java's does, and the array is the same. Since the last element usually belongs near the bottom, it takes about half the compares, as Rust's does.
- An element that compares as one number, which is a number, a `bool`, a `char`, a `rune`, a plain enum, or a struct or tuple of those of up to 128 bits with the derived `Order`, goes down two levels a step. The two children and all four grandchildren are loaded at once, so one wait on memory covers two levels. A struct's members are put side by side in one 64- or 128-bit number, the first member's bits highest, and compared once, with no branch.
- A queue bigger than the second cache, 256 KB, also asks for the lines of the next two levels as it goes down.
- `remove_marked` tags each element with its place, so a mark stays with its element as the elements move, and it keeps the elements that went up past the loop, which Java's iterator keeps in its `forgetMeNot` queue, to take out after the end of the array.

One run of 3 rounds, the best of each, in ms. The `i64`s are 1 million pushed in no order and all popped, then 10 million pops and pushes on a queue of 10,000, each push later than the last pop, as a scheduler's are, then 1 million pushed in order. The ticks are 10 million pops and pushes on a queue of 10,000 structs of a time and an order:

| | push | pop all | steady | in order | ticks steady |
| --- | --- | --- | --- | --- | --- |
| `PriorityQueue<i64>`, `PriorityQueue<Tick>` | 7.4 | 42 | 233 | 1.1 | 325 |
| Rust `BinaryHeap<Reverse<i64>>` | 9.8 | 58 | 269 | 2.7 | 396 |
| Java `PriorityQueue<Long>` | 15 | 271 | 845 | 5.6 | 1320 |
