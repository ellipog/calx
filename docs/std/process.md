---
title: std.process
description: This process's memory, the memory it may use, its CPU time and the processors it may run on.
---

# `std.process`

This process, for a program that shows how it's doing: the memory it holds and may hold, the CPU time it has used, and the processors it may run on.

```calx
import std.process.{memory, memory_limit, processors}

fn main() {
    unsafe {
        let held = memory()
        println("{held > 0} {memory_limit() > held}")    // out: true true
        println("{processors() >= 1}")    // out: true
    }
}
```

| Function | Gives |
| --- | --- |
| `unsafe memory() -> u64` | The bytes of this process's memory in RAM now: its working set on Windows, its resident set on Linux, `top`'s `RES` |
| `unsafe memory_limit() -> u64` | The bytes of RAM it may use: the machine's, or less where a job object's limit on Windows, or a cgroup's on Linux, says |
| `unsafe cpu_time() -> u64` | The nanoseconds of CPU time all its threads have used since it started, in the kernel and out |
| `unsafe processors() -> u32` | The processors it may run on, as `parallel for` counts them |

- **A share of the CPU** over a second is the change in `cpu_time()` over the change in a monotonic clock, divided by `processors()`. Windows adds to the CPU time at each tick of its clock, about every 15.6 ms, so it's for rates over a second or so, not for timing code.
- **The limit in a container** is its cgroup's: on cgroup v2 the lowest `memory.max` of the process's group and those above it, and on v1, `memory.limit_in_bytes`. Where none is set it's the machine's RAM. Each function gives 0 where the system can't say.
- **They're `unsafe`**, as the clock is (D88): each changes as the program runs, or with the machine it runs on.

`std.process` is written in Calx over the runtime: [std/process.calx](../../std/process.calx).
