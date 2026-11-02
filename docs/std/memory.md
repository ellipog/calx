---
title: std.memory
description: Giving the system back the memory freed values held, for a program that's gone quiet after a busy while.
---

# `std.memory`

The memory a program's freed values held, which the allocator keeps for the next ones it makes. Getting memory from the system and touching it the first time is slow, so a freed block waits for the next block of its size ([ownership](../reference/ownership.md#in-c)). After a busy while, such as a server's players flying over new land and then leaving, that can be a lot of memory nothing uses.

```calx
import std.memory.{trim}

fn main() {
    var tables = List<List<String>>()
    for i in 0..1000 {
        var t = List<String>()
        for k in 0..20 {
            t.push("row {i} {k}")
        }
        tables.push(t)
    }
    println("{tables.len()} {tables[999][19]}")    // out: 1000 row 999 19
    tables.clear()
    trim()
    println("{tables.len()}")    // out: 0
}
```

| Item | Does |
| --- | --- |
| `trim()` | Gives the system back what it can of the memory freed values held: each 64 KiB page of small blocks whose blocks are all free, and the big blocks kept for reuse |

How it finds what it can give back:

- **Small blocks** are 32 KiB or less. Each is cut from a 64 KiB page that holds only blocks of its size, and a page knows how many were cut from it.
- **Free blocks** go back to a list kept for each size, a batch at a time, once a thread frees more of a size than it allocates. `trim` counts each page's blocks in that list, holding only that size's lock. A page whose every block is there leaves the list. It goes back to the system, then to whichever size needs a page next.
- **What stays:** each busy thread keeps up to 256 KiB of free blocks of each size for itself, without a lock, so those stay. So does any page with a block still in use.
- **The calling thread** shares its own free blocks first.
- **Big blocks**, over 32 KiB, kept for the next block of their size go back too, up to 256 MiB of them.

The allocator also trims on its own. After every 64 MiB of new pages, a thread that takes a page trims the size whose list holds the most, if that's 16 MiB or more. So memory one size freed goes to the others while the program is busy. `trim()` is for when it's quiet, and nothing takes new pages to set that off.

A thread that has waited on an empty [queue](thread.md#queues) for 2 seconds is idle, as a pool's worker is between bursts. It stops cutting from its pages, shares the free blocks it kept, as it would if it ended, and trims. So the memory idle threads hold goes back without the program asking, and `trim()` from any one thread can give back the pages their blocks held up. Its wait goes on as before. A server whose 31 generator threads went idle after a flight over new land went from 1,225 MB to 861 MB 2 seconds after, and to 428 MB once its own `trim()` ran, where it had stayed at 693 MB.

What `trim` gives back depends on what every thread has freed, and when, so it doesn't say. A program's results never depend on it. Debug builds, and builds with AddressSanitizer, use `malloc`, and there `trim` does nothing.
