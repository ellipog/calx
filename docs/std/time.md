---
title: std.time
description: A monotonic clock for measuring how long something takes, and the wall clock with its dates, which only unsafe code can read.
---

# `std.time`

Two clocks: one that never goes backward, for measuring how long something takes, and the wall clock, for showing the date and time. Reading either needs [`unsafe`](../reference/unsafe.md#unsafe-functions).

## Measuring

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

## The date and time

```calx
import std.time.{Timestamp}

fn two(n: i32) -> String {
    return if n < 10 { "0{n}" } else { "{n}" }
}

fn main() {
    let now = unsafe { Timestamp.now() }
    let local = now.date_time(unsafe { now.local_offset() })
    println("{local.year > 2025}")    // out: true
    let t = Timestamp { unix_ms: 951782400000 }.date_time(-18000)
    println("{t.year}-{two(t.month)}-{two(t.day)} {two(t.hour)}:{two(t.minute)}")    // out: 2000-02-28 19:00
}
```

| Item | Gives |
| --- | --- |
| `Timestamp` | A moment on the wall clock: `unix_ms`, the milliseconds since the start of 1970 in UTC, leap seconds aside, as Java's `System.currentTimeMillis()` counts them. It's `Copy`, and can be made from any `i64` |
| `unsafe Timestamp.now() -> Timestamp` | The wall clock's time now |
| `unsafe t.local_offset() -> i32` | The seconds the system's local time zone is ahead of UTC at `t`, by the zone's rules, so summer time counts. 0 where the system can't say |
| `t.date_time(offset: i32) -> DateTime` | The date and time at `t`, `offset` seconds ahead of UTC |
| `DateTime` | `year`, `month` (1 to 12), `day` (1 to 31), `hour` (0 to 23), `minute`, `second` and `milli` (0 to 999), all `i32` |

The wall clock jumps when the system's clock is set, so measure with an `Instant`. Dates are on the Gregorian calendar, extended back before it began in 1582, with a year 0 before year 1, as `java.time` counts them: `date_time` gives what `Instant.ofEpochMilli(ms).atOffset(ZoneOffset.ofTotalSeconds(offset))` does.

## Why they need `unsafe`

Safe Calx can't see time ([spec](../CALX_SPEC.md#lockstep-ticks)). A program whose results depended on how fast it ran would give different answers on different machines, or from one run to the next, and Calcite's ticks must replay exactly. So the only way to read the clock is an `unsafe fn`, and code that calls it says so. Calcite's tick loop reads it once per tick and records the time with the tick's inputs, and everything else sees that recorded time (D44, D54). A benchmark reads it to time itself.

An `Instant` or a `Timestamp` on its own is harmless: comparing two says nothing new once both are read, and `date_time` is plain arithmetic. The local time zone needs `unsafe` too, since it differs from one machine to the next.

## The clocks

| Platform | `Instant` | `Timestamp` | Local offset |
| --- | --- | --- | --- |
| Windows | `QueryPerformanceCounter`, scaled to nanoseconds by its frequency, which is read once at startup | `GetSystemTimePreciseAsFileTime` | `localtime_s`, then `_mkgmtime64` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | `clock_gettime(CLOCK_REALTIME)` | `localtime_r`'s `tm_gmtoff` |

An `Instant` counts from an arbitrary point, such as when the machine started, and doesn't jump when the wall clock is changed.

`std.time` is written in Calx: [std/time.calx](../../std/time.calx). The clocks themselves are in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
