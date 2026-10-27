---
title: std.pool
description: The pool of threads parallel for runs on, and setting how many threads it uses.
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

The default is the number of logical processors the program may run on: `GetActiveProcessorCount` on Windows, and the CPUs in the program's affinity mask on Linux, so a container limited to some CPUs gets that many.

Setting it doesn't start or stop threads. The next `parallel for` that needs more starts them, and threads beyond the number just don't take part.

`std.pool` is written in Calx: [std/pool.calx](../../std/pool.calx). The pool itself is in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
