---
title: std.time
description: A monotonic clock for measuring how long something takes, the wall clock, which only unsafe code can read, and dates, with their arithmetic, strftime and strptime codes and ISO 8601.
---

# `std.time`

Two clocks: one that never goes backward, for measuring how long something takes, and the wall clock, for showing the date and time. Reading either needs [`unsafe`](../reference/unsafe.md#unsafe-functions). Dates and times of day need nothing: checking them, adding days and months, and [writing and reading them](#formatting-and-parsing) by strftime's codes or in ISO 8601 are plain arithmetic.

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
| `unsafe t.zone_abbreviation() -> String?` | The local time zone's abbreviation at `t`, as the system's time zone database names it, so summer time counts: `CET` or `CEST` in Paris, `+03` in Istanbul, which has none in letters. `none` where the system has none, as on Windows, which names zones only in full |
| `t.date_time(offset: i32) -> DateTime` | The date and time at `t`, `offset` seconds ahead of UTC |
| `DateTime` | `year`, `month` (1 to 12), `day` (1 to 31), `hour` (0 to 23), `minute`, `second` and `milli` (0 to 999), all `i32` |

The wall clock jumps when the system's clock is set, so measure with an `Instant`. Dates are on the Gregorian calendar, extended back before it began in 1582, with a year 0 before year 1, as `java.time` counts them: `date_time` gives what `Instant.ofEpochMilli(ms).atOffset(ZoneOffset.ofTotalSeconds(offset))` does.

## Dates

```calx
import std.time.{DateTime}
import std.time

fn main() {
    let d = DateTime.new(2026, 1, 31, 9, 30)!
    println("{d.weekday()} {d.day_of_year()} {time.is_leap(2028)}")    // out: 5 31 true
    println(d.add_months(1).iso())                                      // out: 2026-02-28T09:30:00
    println(d.add_days(-31).iso())                                      // out: 2025-12-31T09:30:00
    println("{d.timestamp(3600).unix_ms}")                              // out: 1769848200000
    println("{DateTime.new(2026, 2, 29)}")                              // out: none
}
```

| Item | Gives |
| --- | --- |
| `DateTime.new(year, month, day, hour: 0, minute: 0, second: 0, milli: 0) -> DateTime?` | The date and time, or `none` unless each part is in its range, as Python's `datetime(...)` checks them |
| `d.is_valid() -> bool` | Whether each part is in its range, for a `DateTime` made field by field |
| `d.timestamp(offset: i32) -> Timestamp` | The moment `d` is where the clock is `offset` seconds ahead of UTC: the way back from `date_time` |
| `d.weekday() -> i32` | The day of the week, 0 for Monday to 6 for Sunday, as Python's `weekday()` |
| `d.day_of_year() -> i32` | 1 for the 1st of January, up to 365, or 366 in a leap year |
| `d.add_days(n: i64) -> DateTime` | The same time of day `n` days later, or earlier for a negative `n` |
| `d.add_months(n: i32) -> DateTime` | The same day and time `n` months later or earlier, with the day brought back to the month's last when the month is shorter, as Java's `plusMonths` does |
| `is_leap(year: i32) -> bool` | Whether `year` has a 29th of February |
| `days_in_month(year: i32, month: i32) -> i32` | How many days the month has. It panics for a month that isn't from 1 to 12 |

A `DateTime` has no time zone: it's what a clock and calendar show, somewhere. A `Timestamp` is one moment everywhere, so going from one to the other takes an offset, the seconds the clock is ahead of UTC. `add_days` counts calendar days, so across a change to summer time a day later is still the same time of day on the clock.

## Formatting and parsing

```calx
import std.time.{DateTime, Timestamp}

fn main() {
    let d = DateTime.new(2026, 11, 8, 14, 5, 9, 250)!
    println(d.iso())                                    // out: 2026-11-08T14:05:09.250
    println(d.format("%A %d %B %Y, %I:%M %p"))          // out: Sunday 08 November 2026, 02:05 PM
    let p = DateTime.parse("8/11/26 2:05 pm", "%d/%m/%y %I:%M %p")!
    println(p.iso())                                    // out: 2026-11-08T14:05:00
    println("{DateTime.parse("31/11/26", "%d/%m/%y")}") // out: none

    let t = d.timestamp(3600)
    println(t.iso(0))                                   // out: 2026-11-08T13:05:09.250Z
    println(t.format("%F %T %z", -18000))               // out: 2026-11-08 08:05:09 -0500
    let log = Timestamp.parse("08/Nov/2026:14:05:09 +0100", "%d/%b/%Y:%H:%M:%S %z")!
    println("{Timestamp.parse_iso("2026-11-08T13:05:09Z")!.unix_ms == log.unix_ms}")    // out: true
}
```

| Item | Gives |
| --- | --- |
| `d.format(pattern) -> String` | The date and time written by `pattern`'s codes, as Python's `strftime` writes them |
| `d.iso() -> String` | `2026-11-08T14:05:09`, with `.250` after the seconds when there are millis, as Python's `isoformat()` writes it |
| `DateTime.parse(text, pattern) -> DateTime?` | The date and time `text` writes by `pattern`'s codes, as Python's `strptime` reads it |
| `DateTime.parse_iso(text) -> DateTime?` | A date and time in ISO 8601's form with no offset: `2026-11-08`, `2026-11-08T14`, `2026-11-08T14:05`, `2026-11-08 14:05:09.250` |
| `t.format(pattern, offset: i32) -> String` | The moment written by `pattern`'s codes at `offset` seconds ahead of UTC, with `%z` for the offset |
| `t.iso(offset: i32) -> String` | The moment as RFC 3339 writes it: `2026-11-08T14:05:09+01:00`, or with `Z` for UTC |
| `Timestamp.parse(text, pattern) -> Timestamp?` | The moment `text` writes by `pattern`'s codes, which must have a `%z` |
| `Timestamp.parse_iso(text) -> Timestamp?` | A date and time as `DateTime.parse_iso` reads them, then `Z` or an offset: `+01:00`, `+0100` or `+01` |

| Code | Writes | Code | Writes |
| --- | --- | --- | --- |
| `%Y` | The year, in four digits or more: `2026` | `%y` | The year's last two digits: `26` |
| `%m` | The month, `01` to `12` | `%d` | The day of the month, `01` to `31` |
| `%B` | The month's name: `November` | `%b` | Its first three letters: `Nov` |
| `%A` | The day's name: `Sunday` | `%a` | Its first three letters: `Sun` |
| `%u` | The day of the week, `1` for Monday to `7` | `%w` | The day of the week, `0` for Sunday to `6` |
| `%j` | The day of the year, `001` to `366` | `%H` | The hour, `00` to `23` |
| `%I` | The hour on a 12-hour clock, `01` to `12` | `%p` | `AM` or `PM` |
| `%M` | The minute, `00` to `59` | `%S` | The second, `00` to `59` |
| `%f` | The microseconds, in six digits: `250000` | `%%` | A `%` |
| `%F` | `%Y-%m-%d` | `%T` | `%H:%M:%S` |
| `%z` | The offset, `+0100`; only for a `Timestamp` | `%:z` | The offset with a colon, `+01:00` |

Any other character in a pattern is written as it is, and any other code panics, since a pattern is part of the program, not its input. The names are in English, whatever the system's language.

`parse` reads what `format` writes, by Python's rules:
- `%Y` reads four digits, `%y` two, and `%f` one to six. The other numbers are one digit or two, and the names are read in any case.
- `%y` reads `69` to `99` as 1969 to 1999 and `00` to `68` as 2000 to 2068.
- A space in the pattern matches any run of whitespace, but at least one. Any other character must be in the text as it is.
- `%p` changes the hour only when `%I` read it.
- `%j` sets the month and day from the day of the year.
- A part the pattern doesn't give is as in the 1st of January 1900, at midnight.
- `%z` reads `Z`, or a sign, the hours, then the minutes, with a `:` between or none.
- The whole text must match and the date and time must be in range, or `parse` gives `none`.

Calx and Python 3.14 parse differently in three places:
- Calx gives `none` for day 366 of a year with 365, where Python's `strptime` rolls it over into the next year.
- Calx reads `%:z` as `%z`, which Python's `strptime` doesn't have.
- `parse_iso` reads a lowercase `t` and `z`, as RFC 3339 allows, but not `24:00` or the forms without dashes and colons, such as `20261108`.

A `Timestamp` pattern needs a `%z`, or `Timestamp.parse` panics: a time with no offset could be any moment, so read it with `DateTime.parse` and give it an offset with `timestamp`.

## Why they need `unsafe`

Safe Calx can't see time ([spec](../CALX_SPEC.md#lockstep-ticks)). A program whose results depended on how fast it ran would give different answers on different machines, or from one run to the next, and a program that replays its inputs couldn't replay its results. So the only way to read the clock is an `unsafe fn`, and code that calls it says so. A program that must replay, such as a game server's tick loop, reads it once per step and records the time with that step's inputs, and everything else sees that recorded time (D44, D54). A benchmark reads it to time itself.

An `Instant` or a `Timestamp` on its own is harmless: comparing two says nothing new once both are read, and `date_time`, `format` and the rest are plain arithmetic. The local time zone needs `unsafe` too, since it differs from one machine to the next.

## The clocks

| Platform | `Instant` | `Timestamp` | Local offset |
| --- | --- | --- | --- |
| Windows | `QueryPerformanceCounter`, scaled to nanoseconds by its frequency, which is read once at startup | `GetSystemTimePreciseAsFileTime` | `localtime_s`, then `_mkgmtime64` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | `clock_gettime(CLOCK_REALTIME)` | `localtime_r`'s `tm_gmtoff` |

An `Instant` counts from an arbitrary point, such as when the machine started, and doesn't jump when the wall clock is changed.

`std.time` is written in Calx: [std/time.calx](../../std/time.calx). The clocks themselves are in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
