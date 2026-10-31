---
title: std.hint
description: The black_box function, which hides a value from the optimizer so a benchmark measures what it means to.
---

# `std.hint`

Hints to the optimizer, for benchmarks. Release builds optimize the whole program at once (D99), so clang sees through calls between modules: a loop that only sums what a function returns can be moved out from between two readings of the clock, or worked out while the program builds. `black_box` stops that.

```calx
import std.hint.{black_box}
import std.time.{Instant}

fn cube(x: f64) -> f64 {
    return x * x * x
}

fn main() {
    let start = unsafe { Instant.now() }
    var acc = 0.0
    for i in 0..1000 {
        acc += cube(f64(i))
    }
    acc = black_box(acc)    // the loop runs before the clock is read again
    let ns = unsafe { Instant.now() }.nanos_since(start)
    println("{acc} {ns < 1000000000}")    // out: 2.4950025E11 true
}
```

| Item | Does |
| --- | --- |
| `black_box<T>(x: own T) -> T` | Gives back `x`, in a way clang can't see through. It can't know the result, so it can't work out in advance what depends on it, and it must have `x` ready by the call, so it can't drop the work that made `x` or move that work past the call |

- It takes any type, and moves its argument in and out, so `xs = black_box(xs)` works on a list too.
- It costs about one store and one load: in C, it's an empty `asm volatile` that clang must assume reads `x`'s memory and writes any memory. It changes no result, in either build mode.
- Pass what a loop computes through it once, after the loop, rather than every value inside it, which would measure the stores too.
