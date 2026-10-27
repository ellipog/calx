---
title: std.time
description: A monotonic clock for measuring how long something takes, which only unsafe code can read.
---

# `std.time`

A clock that never goes backward, for measuring how long something takes. Reading it needs [`unsafe`](../reference/unsafe.md#unsafe-functions).

```calx
import std.time.{Instant}

fn work() -> i64 {
    var x: i64 = 1
    for i in 0..100000 {
        x = x * 31 + i64(i)
    }
    return x
}

fn main() {
    let start = unsafe { Instant.now() }
    let x = work()
    let end = unsafe { Instant.now() }
    let ns = end.nanos_since(start)
    println("{x != 0} {ns < 60000000000} {end.secs_since(start) < 60.0}")    // out: true true true
}
```

| Item | Gives |
| --- | --- |
| `Instant` | A moment on the clock, to the nanosecond. It's `Copy`, and means something only next to another `Instant` from the same run of the program |
| `unsafe Instant.now() -> Instant` | The clock's time now |
| `t.nanos_since(earlier: Instant) -> u64` | The nanoseconds from `earlier` to `t`, or 0 if `earlier` is later |
| `t.secs_since(earlier: Instant) -> f64` | The same in seconds |

## Why it needs `unsafe`

Safe Calx can't see time ([spec](../CALX_SPEC.md#lockstep-ticks)). A program whose results depended on how fast it ran would give different answers on different machines, or from one run to the next, and Calcite's ticks must replay exactly. So the only way to read the clock is an `unsafe fn`, and code that calls it says so. Calcite's tick loop reads it once per tick and records the time with the tick's inputs, and everything else sees that recorded time (D44, D54). A benchmark reads it to time itself.

An `Instant` on its own is harmless: comparing two says nothing new once both are read.

## The clock

| Platform | Source |
| --- | --- |
| Windows | `QueryPerformanceCounter`, scaled to nanoseconds by its frequency, which is read once at startup |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` |

Both count from an arbitrary point, such as when the machine started, and don't jump when the wall clock is changed.

`std.time` is written in Calx: [std/time.calx](../../std/time.calx). The clock itself is in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
