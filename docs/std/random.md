---
title: std.random
description: Random numbers from a seed, the same on every system, or seeded from the system for runs that differ - integers, floats, normal values, shuffles, picks and samples.
---

# `std.random`

A fast generator of random numbers, for simulations, games, tests and sampling. The same seed gives the same numbers on every system, every build mode and every run. `Random.system()` seeds one from the system's own generator instead, when each run should differ.

```calx
import std.random.{Random}

fn main() {
    var r = Random.new(42)
    println("{r.below(6) < 6} {r.range(-3, 3) >= -3}")    // out: true true
    let roll = r.float()
    println("{roll >= 0.0 and roll < 1.0}")               // out: true
    var deck: List<i32> = [1, 2, 3, 4, 5]
    r.shuffle(mut deck)
    deck.sort()
    println("{deck}")                                     // out: [1, 2, 3, 4, 5]
    let names: List<String> = ["ann", "bob", "cy"]
    println("{r.sample(names, 2).len()}")                 // out: 2
    var a = Random.new(7)
    var b = Random.new(7)
    println("{a.next_u64() == b.next_u64()}")             // out: true
}
```

| Item | Gives |
| --- | --- |
| `Random.new(seed: u64)` | A generator whose numbers follow from `seed` |
| `Random.system()` | A generator seeded from the system's generator, as [`std.crypto.random_bytes`](crypto.md) reads it |
| `r.next_u64()`, `r.next_u32()` | The next 64 or 32 random bits |
| `r.below(n: u64)` | A number from 0 up to, not including, `n`. It panics when `n` is 0 |
| `r.range(lo: i64, hi: i64)` | A number from `lo` up to, not including, `hi`, as Python's `randrange(lo, hi)`. It panics unless `lo < hi` |
| `r.index(len: usize)` | An index into a list of `len` elements |
| `r.float()` | A float from 0 up to, not including, 1, as Python's `random()` |
| `r.uniform(lo: f64, hi: f64)` | A float from `lo` up to `hi` |
| `r.coin()` | `true` or `false`, each half the time |
| `r.chance(p: f64)` | `true` with probability `p` |
| `r.normal(mean: f64, sd: f64)` | A number from the normal distribution, as Python's `gauss(mean, sd)` |
| `r.shuffle(mut xs)` | Puts a list in a random order |
| `r.pick(xs)` | A copy of a random element, or `none` for an empty list, as Python's `choice` |
| `r.sample(xs, k)` | Copies of `k` different elements, in the order they were picked. It panics when the list has fewer |
| `r.bytes(n)` | `n` random bytes |

- Every method that takes numbers from the generator takes `mut self`, so a generator lives in a `var` and is passed on as `mut r`.
- Each integer in a range is as likely as the others: `below` uses Lemire's method, the high half of a random number times `n` from [`widening_mul`](../reference/integers.md#methods), retried only in the rare case that would favor some results. `float` takes 53 random bits, so its values are evenly spaced, `normal` is Marsaglia's polar method, and `shuffle` and `sample` are Fisher and Yates's.
- The numbers aren't for keys, tokens or passwords: a few of them are enough to work out the rest. Those come from [`std.crypto.random_bytes`](crypto.md).
- `Random.system()` is the one thing in the module that differs between runs. A program that should give the same results every time, as Calcite's worlds do, seeds with `Random.new`.

## The generator

It's xoshiro256** (Blackman and Vigna, 2018): 256 bits of state, which repeat only after 2^256 - 1 numbers, and which its authors report passes the BigCrush and PractRand test suites. A step is a few shifts, rotations and xors and two multiplies, with no branch. `Random.new` spreads the seed over the state with SplitMix64, as the authors recommend, so nearby seeds give unrelated numbers.
