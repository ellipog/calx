---
title: std.thread
description: Threads that run alongside the program, the Task they run, and the typed queues they talk through.
---

# `std.thread`

Threads that run alongside the rest of a program, and the queues they talk through (D121).

[`parallel for`](../reference/parallel.md) is how Calx makes work faster, and it gives the same result on any number of threads (D21). A thread from `std.thread` is for something else: a service that waits on the network or the disk while the program goes on, as Calcite's network and disk threads do. What a thread does when depends on timing, so starting one takes [`unsafe`](../reference/unsafe.md), as reading the clock does (D88).

```calx
import std.thread.{Task}

struct Squarer {
    jobs: Receiver<i32>
    out: Sender<String>
}

impl Task for Squarer {
    fn run(own self) {
        while let n = self.jobs.wait() {
            self.out.send("{n} squared is {n * n}")
        }
    }
}

fn main() {
    let jobs = Receiver<i32>()
    var results = Receiver<String>()
    var to_worker = jobs.sender()
    let t = unsafe { Thread.spawn(Squarer { jobs: jobs, out: results.sender() }) }
    for i in 1..=3 {
        to_worker.send(i)
    }
    to_worker.close()
    while let s = results.wait() {
        println(s)
    }
    // out: 1 squared is 1
    // out: 2 squared is 4
    // out: 3 squared is 9
    t.join()
}
```

The worker ends when `wait` gives `none`, once `to_worker` is closed and the queue is empty. Its `out` goes with it, so the loop in `main` ends then too.

## Threads

A thread runs a task: a value whose type implements `Task`, which moves to the new thread. The thread calls the task's `run`, and ends when `run` returns.

| Item | Does |
| --- | --- |
| `interface Task { fn run(own self) }` | What a thread does. `run` owns the task, so the thread owns everything in it |
| `Thread.spawn(task)` | Starts a thread that runs `task.run()`, and gives its `Thread`. Needs `unsafe`. The task's type must be `Send` |
| `t.join()` | Waits for the thread to end, and uses `t` up |

Every thread's stack is 8 MiB, the main thread's too, on every system, so a program recurses as deep on Windows, Linux and macOS, and on any of its threads (D135). It's address space: memory is taken only as the stack grows into it. A call that would go past the end [panics](../reference/printing.md#panics), at `<stack>`, as any other bug does.

A `Thread` that's dropped without `join()` carries on by itself, and ends with the program if it hasn't ended before. Memory such a thread still holds when the program ends isn't reported as a leak, since it's still in use.

A panic on any thread ends the whole program, as one on the main thread does. Its message and [stack trace](../reference/printing.md#stack-traces) are the panicking thread's, and everything printed before it is written first. Lines printed from different threads never mix: each `println` is written whole.

A `parallel for` on a thread that `Thread.spawn` started runs on that thread alone, so services don't compete with the program's own loops for the pool. It gives the same result anyway. `set_workers` panics on such a thread.

## Queues

A queue carries messages of one type, `T`, which must be `Send`. Each message moves into the queue when it's sent, and out when it's taken, so it has one owner the whole way. Messages from one `Sender` come out in the order they went in.

`Receiver<T>()` makes a queue, and gives the end that takes its messages. `sender()` makes an end that sends. Either kind of end can be shared with `share()`, which makes another end of the same queue: several threads can send to one queue, and several can take from it, each taking different messages.

| Item | Does |
| --- | --- |
| `Receiver<T>()` | A new, empty queue of `T`, and its first receiving end. The type can come from the variable's, as in `let r: Receiver<i32> = Receiver()` |
| `r.sender()` | A new end that sends to `r`'s queue |
| `s.send(m)` | Moves `m` into the queue. If every receiving end is gone, `m` is dropped instead |
| `r.next()` | The first message, or `none` at once if there isn't one |
| `r.wait()` | The first message, waiting for one if the queue is empty. Gives `none` once it's empty and every `Sender` is gone |
| `s.share()`, `r.share()` | Another end of the same kind, for the same queue |
| `s.close()`, `r.close()` | Drops the end now, as going out of scope would |

`send`, `next` and `wait` change the queue, so they're called on a `var`, or a `mut` or `own` parameter. A queue grows as messages come, so `send` never waits.

Messages still in a queue when its last receiving end goes are dropped then. Once every end of a queue is gone, its memory is freed.

```calx
fn main() {
    var r = Receiver<String>()
    println("{r.next()}")    // out: none
    var a = r.sender()
    var b = a.share()
    a.send("one")
    b.send("two")
    a.close()
    b.close()
    println("{r.wait()} {r.wait()} {r.wait()}")    // out: some(one) some(two) none
}
```

## What it doesn't allow

- A `Sender`, a `Receiver` and a `Thread` can't be copied, compared, shown or hashed. Each is a handle to something threads share, and `share()` is how to get another end.
- A queue can't be used, and a thread can't be joined, inside a `parallel for`: which iteration's message came first would depend on timing. Writing it in the body is an error, and reaching it from a function the body calls panics, with one thread as with many.
- Threads share no memory: every value has one owner (D76). A task, and every message, must be `Send`, so it holds no pointers, and a generic one needs the bound `T: Send`.

`std.thread` is written in Calx: [std/thread.calx](../../std/thread.calx). The threads and queues are in the runtime, [runtime/calx_thread.c](../../runtime/calx_thread.c), which is linked only into programs that use them.
