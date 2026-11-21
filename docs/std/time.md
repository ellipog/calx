---
title: std.time
description: A monotonic clock for measuring how long something takes, the wall clock, which only unsafe code can read, dates, with their arithmetic, strftime and strptime codes and ISO 8601, and the IANA time zones, with summer time and their history, built in.
---

# `std.time`

Two clocks: one that never goes backward, for measuring how long something takes, and the wall clock, for showing the date and time. Reading either needs [`unsafe`](../reference/unsafe.md#unsafe-functions). Dates and times of day need nothing: checking them, adding days and months, and [writing and reading them](#formatting-and-parsing) by strftime's codes or in ISO 8601 are plain arithmetic. So are [time zones](#time-zones): the IANA tz database is in the program, so `Europe/Oslo`'s summer time is the same on every system, Windows too.

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

A `DateTime` has no time zone: it's what a clock and calendar show, somewhere. A `Timestamp` is one moment everywhere, so going from one to the other takes an offset, the seconds the clock is ahead of UTC, or a [`TimeZone`](#time-zones), which knows the offset at each moment. `add_days` counts calendar days, so across a change to summer time a day later is still the same time of day on the clock.

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

A `Timestamp` pattern needs a `%z`, or `Timestamp.parse` panics: a time with no offset could be any moment, so read it with `DateTime.parse` and give it an offset with `timestamp`, or a zone with [`zone.timestamp`](#time-zones).

## Time zones

```calx
import std.time.{DateTime, TimeZone, Timestamp}

// The next moment after `now` that it's 08:00 on the zone's clock.
fn next_eight(zone: TimeZone, now: Timestamp) -> Timestamp {
    var day = zone.date_time(now)
    day.hour = 8
    day.minute = 0
    day.second = 0
    day.milli = 0
    let t = zone.timestamp(day)
    return if t.unix_ms > now.unix_ms { t } else { zone.timestamp(day.add_days(1)) }
}

fn main() {
    let oslo = TimeZone.named("Europe/Oslo")!
    let summer = Timestamp { unix_ms: 1782000000000 }
    println("{oslo.offset(summer)} {oslo.abbreviation(summer)} {oslo.is_dst(summer)}")   // out: 7200 CEST true
    println(oslo.date_time(summer).iso())                                                 // out: 2026-06-21T02:00:00
    println(summer.iso(oslo.offset(summer)))                                              // out: 2026-06-21T02:00:00+02:00

    // Oslo goes to summer time on the 29th of March 2026, so 08:00 comes an hour sooner.
    let before = DateTime.new(2026, 3, 28, 12)!.timestamp(0)
    let first = next_eight(oslo, before)
    println("{first.iso(0)} {next_eight(oslo, first).iso(0)}")                            // out: 2026-03-29T06:00:00Z 2026-03-30T06:00:00Z

    // 02:30 that night never shows on the clock, so it moves on an hour, to 03:30.
    let gap = oslo.timestamp(DateTime.new(2026, 3, 29, 2, 30)!)
    println(gap.iso(oslo.offset(gap)))                                                    // out: 2026-03-29T03:30:00+02:00
    // 02:30 on the 25th of October shows twice, and the first is still summer time.
    let twice = oslo.timestamp(DateTime.new(2026, 10, 25, 2, 30)!)
    println(twice.iso(oslo.offset(twice)))                                                // out: 2026-10-25T02:30:00+02:00

    let sydney = TimeZone.named("Australia/Sydney")!
    println("{sydney.abbreviation(summer)} {TimeZone.named("Asia/Kolkata")!.offset(summer)}")  // out: AEST 19800
    println("{TimeZone.named("Mars/Olympus_Mons")} {TimeZone.utc().offset(summer)}")      // out: none 0
}
```

| Item | Gives |
| --- | --- |
| `TimeZone.named(name: String) -> TimeZone?` | The zone the IANA tz database names `name`, such as `Europe/Oslo`, `America/New_York` or `UTC`, as Python's `ZoneInfo(name)` and Java's `ZoneId.of(name)` find it, or `none` for a name it hasn't got. Names are matched exactly, in their case, and the old names the database keeps, such as `US/Eastern` and `Asia/Calcutta`, work too |
| `TimeZone.utc() -> TimeZone` | UTC, named `UTC` |
| `TimeZone.names() -> List<String>` | Every name `named` finds, in order of their bytes, as Python's `zoneinfo.available_timezones()` gives them: 597 in 2026f |
| `TZDATA_VERSION: str` | The tz database's version, `2026f` |
| `z.name() -> String` | The name it was found by |
| `z.offset(t: Timestamp) -> i32` | The seconds its clock is ahead of UTC at `t`, by its rules, so summer time and every past change count: 3600 in Oslo in winter and 7200 in summer, 19800 in Kolkata |
| `z.abbreviation(t: Timestamp) -> String` | Its abbreviation at `t`, as the database has it: `CET` or `CEST`, `EST` or `EDT`, `LMT` before standard time came, and the offset where the zone has none in letters, as `+03` in Istanbul |
| `z.is_dst(t: Timestamp) -> bool` | Whether it's on summer time at `t` |
| `z.date_time(t: Timestamp) -> DateTime` | What its clock and calendar show at `t`: `t.date_time(z.offset(t))` |
| `z.timestamp(d: DateTime) -> Timestamp` | The moment its clock shows `d`, with a gap or an overlap as below |

A `TimeZone` is `Copy`, and needs no `unsafe`: its rules are data in the program, the same on every machine, not something the machine says. Only the system's own zone does, through `local_offset`.

### Gaps and overlaps

When the clocks go forward, a stretch of wall-clock times never shows: in Oslo, 02:00 to 03:00 on the last Sunday of March. When they go back, a stretch shows twice. `timestamp` gives:

- **In a gap**, the time moved forward by the gap's length, as java.time's `ZonedDateTime.of` does: 02:30 is 03:30 summer time, which is the moment 02:30 would be on the clock from before the change.
- **In an overlap**, the earlier of the two moments, on the offset from before the change, as java.time does.

Both are what Python gives for `datetime(..., tzinfo=ZoneInfo(name)).timestamp()`, with `fold=0`. A day with a change has 23 or 25 hours, so "daily at 08:00" is best worked out from the date, as `next_eight` above does, rather than by adding 86,400,000 ms to the last run.

### The tz database

The zones are the [IANA tz database](https://www.iana.org/time-zones)'s, release 2026f (2026-10-27), built into the standard library: [std/time/zones.bin](../../std/time/zones.bin), 289 KB, with each zone's TZif file (RFC 9636) and its names. Only a program that calls `TimeZone` carries it, since the link drops a table nothing reads. Nothing is read from the system, so there's no `/usr/share/zoneinfo` to need, on Windows or anywhere, and a program gives the same answers on every system, whatever release of the database the system has. A program built with a newer Calx gets a newer release.

- The names are the database's 597: its 344 zones, and the links from old or other names to them, as Python's `tzdata` package has them. Since 2022 the database merges zones that have agreed since 1970, so `Europe/Oslo` leads to `Europe/Berlin`'s data, and its offsets before 1970 are Berlin's.
- Transitions are given up to 2037, and after that each zone's rule, the POSIX TZ string at the end of its file, such as `CET-1CEST,M3.5.0,M10.5.0/3`, works out each year's.
- The database is in its main form, so Ireland's standard time is its summer time and `is_dst` is true there in winter, as Python's `dst()` also says.
- There are no leap seconds, as there are none in a `Timestamp`.

[tools/tz/gen_zones.calx](../../tools/tz/gen_zones.calx) builds the table: it downloads a release, checks its SHA-256, and compiles it with zic, the database's own compiler. To move to a new release, check its signature, and change the version and SHA-256 at the top of the tool.

Over the 597 names, 360,459 moments and 624,064 wall-clock times were checked against Python 3.14's `zoneinfo` reading the same release: random ones from 1800 to 2200, and those a second around each transition and around each gap and overlap. All agree, but for 16 moments a second after a zone's only transition, such as Riyadh's in 1947, where Python still gives the old offset, and zdump, the database's own reader, agrees with Calx.

`named` takes about 100 ns, `offset` 36 ns and `timestamp` 100 ns, in a release build on an i9-13900K: a binary search of the names, then of the zone's transitions, or the year's two from its rule.

## Why they need `unsafe`

Safe Calx can't see time ([spec](../CALX_SPEC.md#lockstep-ticks)). A program whose results depended on how fast it ran would give different answers on different machines, or from one run to the next, and a program that replays its inputs couldn't replay its results. So the only way to read the clock is an `unsafe fn`, and code that calls it says so. A program that must replay, such as a game server's tick loop, reads it once per step and records the time with that step's inputs, and everything else sees that recorded time (D44, D54). A benchmark reads it to time itself.

An `Instant` or a `Timestamp` on its own is harmless: comparing two says nothing new once both are read, and `date_time`, `format` and the rest are plain arithmetic. The local time zone needs `unsafe` too, since it differs from one machine to the next. A `TimeZone` doesn't, since its rules come with the program.

## The clocks

| Platform | `Instant` | `Timestamp` | Local offset |
| --- | --- | --- | --- |
| Windows | `QueryPerformanceCounter`, scaled to nanoseconds by its frequency, which is read once at startup | `GetSystemTimePreciseAsFileTime` | `localtime_s`, then `_mkgmtime64` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | `clock_gettime(CLOCK_REALTIME)` | `localtime_r`'s `tm_gmtoff` |

An `Instant` counts from an arbitrary point, such as when the machine started, and doesn't jump when the wall clock is changed.

`std.time` is written in Calx: [std/time.calx](../../std/time.calx), with the tz database in [std/time/zones.calx](../../std/time/zones.calx). The clocks themselves are in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c).
