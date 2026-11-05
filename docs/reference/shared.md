---
title: Shared values
description: Shared<T>, one value that many owners and threads read at once without copying it - making one, the read-only place s.value, counted copies, and its C.
---

# Shared values

`Shared<T>` holds one value that every copy of it reads, and that nothing changes. Copying a `Shared` doesn't copy the value: it counts one more owner, and the last owner dropped frees the value. So data that many threads or parts of a program read, such as a game's registries, its configuration or the tables a world generator reads, is in memory once, however many hold it:

```calx
import std.thread.{Task}

struct Tables {
    name: String
    scale: i64
}

struct Worker {
    tables: Shared<Tables>
    from: i64
    out: Sender<i64>
}

impl Task for Worker {
    fn run(own self) {
        self.out.send(self.from * self.tables.value.scale)
    }
}

fn main() {
    let tables = Shared(Tables { name: String("overworld"), scale: 3 })
    var results = Receiver<i64>()
    var threads = List<Thread>()
    for i in 1..=4 {
        threads.push(unsafe { Thread.spawn(Worker { tables: tables.copy(), from: i64(i), out: results.sender() }) })
    }
    var sum: i64 = 0
    for _ in 0..4 {
        sum += results.wait() ?? 0
    }
    while let t = threads.pop() {
        t.join()
    }
    println("{tables.value.name} {sum}")    // out: overworld 30
}
```

`Shared(v)` makes one holding `v`, and `Shared<T>(v)` names what it holds.

## The value

`s.value` is the value's place, as a box's is, but it's only read: it's passed as a view, compared, shown and gone over where it is, from any thread with a copy, at the same time as the others. Nothing changes it, through a field, a method that takes `mut self`, a `mut` argument or `for x in mut`, and it can't be moved out. To change it, change a copy of it and share that:

```calx
struct Config {
    view: i32
    names: List<String>
}

fn widest(c: Config) -> usize {
    var most: usize = 0
    for n in c.names {
        if n.len() > most {
            most = n.len()
        }
    }
    return most
}

fn main() {
    var config = Shared(Config { view: 10, names: [String("spawn"), String("nether")] })
    let old = config.copy()
    var next = config.value.copy()
    next.view = 12
    config = Shared(next)
    println("{config.value.view} {old.value.view} {widest(config.value)}")    // out: 12 10 6
}
```

Each copy keeps the value it was made from, so `old` still reads the first config after `config` shares a new one.

A `Shared` is compared, ordered and hashed as its value, so it can be a key of a native `Map` or `Set`, and it's shown as its value:

```calx
fn main() {
    var seen = Set<Shared<String>>()
    seen.insert(Shared(String("stone")))
    seen.insert(Shared(String("stone")))
    println("{seen.len()} {Shared(3) == Shared(3)} {Shared(String("dirt"))}")    // out: 1 true dirt
}
```

## What it holds

A `Shared` holds a value that's `Send` ([threads](../std/thread.md)), since every thread with a copy reads it: not a pointer or an arena list, or a value that holds one. A generic type that shares a `T` gives `T` the bound `Send`:

```calx
struct Cache<T: Send> {
    items: Shared<List<T>>
}

fn main() {
    let a = Cache<i32> { items: Shared([1, 2, 3]) }
    let b = Cache<i32> { items: a.items.copy() }
    println("{b.items.value.len()} {a.items}")    // out: 3 [1, 2, 3]
}
```

A `Shared` is `Send` too, so a copy goes to a thread in a message or a task, and a `parallel for` reads one, and copies it, from every iteration.

Like a [box](boxes.md), it isn't a list: it isn't indexed, gone over in a loop or written as a list literal, it has no methods but `copy()` and no fields but `value`, and it has no `JavaHash`.

## In C

A `Shared` is a box whose memory starts with the count of its owners, 16 bytes before the value, so the value stays aligned. `copy()` adds one to the count with an atomic add, and dropping a copy takes one away. The owner that takes it to zero frees what the value owns, then the memory, and since the count is changed with acquire and release, that owner sees every read the others made before they let go. `s.value` is read in place, as a box's value is, without a bounds check.
