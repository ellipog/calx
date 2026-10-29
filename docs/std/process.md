---
title: std.process
description: This process's memory, the memory it may use, its CPU time and the processors it may run on, and ending it with a status.
---

# `std.process`

This process, for a program that shows how it's doing: the memory it holds and may hold, the CPU time it has used, and the processors it may run on. And `exit`, which ends it with a status.

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
| `exit(code: u8)` | Doesn't return: it ends the program now, with the status `code` |

- **A share of the CPU** over a second is the change in `cpu_time()` over the change in a monotonic clock, divided by `processors()`. Windows adds to the CPU time at each tick of its clock, about every 15.6 ms, so it's for rates over a second or so, not for timing code.
- **The limit in a container** is its cgroup's: on cgroup v2 the lowest `memory.max` of the process's group and those above it, and on v1, `memory.limit_in_bytes`. Where none is set it's the machine's RAM. Each function gives 0 where the system can't say.
- **They're `unsafe`**, as the clock is (D88): each changes as the program runs, or with the machine it runs on. `exit` isn't: it reads nothing.
- **`exit` ends the program from any thread**, as returning from `main` does: the terminal is put back, with what [`std.term.at_exit`](term.md) says, and what was printed is written. Nothing else runs: other threads stop where they are, no value is dropped, and a debug build doesn't check for leaks. The status is the same on every system, since it's a `u8`. Inside a `parallel for` it [panics](../reference/printing.md#panics), at `<exit>`, since which iteration ended the program would depend on timing.

`std.process` is written in Calx over the runtime: [std/process.calx](../../std/process.calx).
