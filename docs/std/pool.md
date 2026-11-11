---
title: std.pool
description: The pool of threads parallel for runs on, setting how many threads it uses, and pools of a program's own for parallel(pool) for.
---

# `std.pool`

The pool of threads that [`parallel for`](../reference/parallel.md) runs on. A `parallel for` gives the same result with any number of threads (D21), so the number changes only how fast a program runs.

```calx
import std.pool.{set_workers}

fn main() {
    set_workers(4)
    var squares = [0; 8]
    parallel for k in 0usize..8 {
        squares[k] = i32.wrap(k * k)
    }
    println("{squares[7]}")    // out: 49
}
```

| Item | Does |
| --- | --- |
| `set_workers(n: usize)` | Makes each `parallel for` run on `n` threads, counting the thread that reaches it. With 1, every `parallel for` runs on that thread alone. Panics if `n` is 0, or if called inside a `parallel for` |
| `unsafe workers() -> usize` | How many threads each `parallel for` runs on: the default below, or what `set_workers` set. It's `unsafe`, as reading the clock is, since it differs from machine to machine: a program can size its own threads by it, such as threads that compress what a server sends, but what it prints with it isn't the same everywhere |

The default is the number of logical processors the program may run on: `GetActiveProcessorCount` on Windows, and the CPUs in the program's affinity mask on Linux, so a container limited to some CPUs gets that many.

```calx
import std.pool.{set_workers, workers}

fn main() {
    set_workers(3)
    println("{unsafe { workers() }}")    // out: 3
    println("{unsafe { workers() } > 0}")    // out: true
}
```

Setting it doesn't start or stop threads. The next `parallel for` that needs more starts them, and threads beyond the number just don't take part.

## Pools of its own

A `Pool` is a set of threads of the program's own, which [`parallel(pool) for`](../reference/parallel.md#on-a-pool-of-its-own) runs a loop on instead of the main pool (CLX-71). A thread that [`Thread.spawn`](thread.md) started runs its loops on that thread alone, so a pool is how it runs them on threads. It's also how work that mustn't take the main pool's threads gets its own, such as a world generator's beside a server's ticks.

```calx
import std.pool.{Pool}
import std.thread.{Task}

struct Generate {
    pool: Pool
    out: Sender<i64>
}

impl Task for Generate {
    fn run(own self) {
        var chunk: List<i64> = [0; 256]
        parallel(self.pool) for k in 0usize..chunk.len() {
            chunk[k] = i64.wrap(k) * 2
        }
        var sum: i64 = 0
        for x in chunk {
            sum += x
        }
        self.out.send(sum)
        self.pool.close()
    }
}

fn main() {
    var done = Receiver<i64>()
    let t = unsafe { Thread.spawn(Generate { pool: Pool.new(4), out: done.sender() }) }
    println("{done.wait()}")    // out: some(65280)
    t.join()
}
```

| Item | Does |
| --- | --- |
| `Pool.new(threads: usize) -> Pool` | A pool whose loops run on `threads` threads, counting the thread that runs the loop, so it starts `threads - 1` of its own, with its first loop. With 1, its loops run on that thread alone. Panics if `threads` is 0 |
| `p.threads() -> usize` | How many threads its loops run on, counting the thread that runs each one |
| `p.close()` | Stops its threads once no loop runs on it, and frees it |

- A `Pool` is a [handle](../reference/ownership.md#handles), so it isn't copied: there's one set of threads, and one thing to close them. A pool that's never closed keeps its threads, asleep, until the program ends.
- It's `Send`, so it moves to the thread that runs loops on it. One thread at a time runs a loop on a pool; another waits until that's done.
- `set_workers` sets the main pool's threads only.
- A pool's threads wait for its next loop as the main pool's do: they spin for a fifth of a millisecond, then sleep.

`std.pool` is written in Calx: [std/pool.calx](../../std/pool.calx). The pool itself is in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
