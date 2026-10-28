---
title: Parallel loops
description: parallel for, the capture check that keeps iterations apart, why results don't depend on the thread count, and its C.
---

# Parallel loops

`parallel for` runs a loop's iterations at once, on a pool of threads. It's the only parallelism in Calx (D21), and the checker makes sure the iterations can't touch the same memory, so a program gives the same result with one thread or with many.

```calx
struct Region {
    blocks: List<i32>,
    total: i64,
}

fn tick(r: mut Region, scale: i32) {
    for i in 0usize..r.blocks.len() {
        r.blocks[i] = r.blocks[i] * scale + 1
        r.total += i64(r.blocks[i])
    }
}

fn main() {
    var regions = List<Region>()
    for k in 0..16 {
        regions.push(Region { blocks: [k, k + 1, k + 2], total: 0 })
    }
    let scale = 3
    parallel for k in 0usize..regions.len() {
        tick(mut regions[k], scale)
    }
    println("{regions[0].total} {regions[15].total}")    // out: 12 147
}
```

The loop goes over a range of integers, as a `for` loop can. Each iteration gets its own value of the loop variable, here `k`, and the loop returns once every iteration has finished.

## Over the elements

A `parallel for` can also go over the elements of a list or an array. With `mut`, each iteration gets a view of its own element and changes it, as a [`for x in mut xs`](control-flow.md#changing-the-elements) loop does (D116):

```calx
struct Region {
    blocks: List<i32>,
    total: i64,
}

fn main() {
    var regions: List<Region> = []
    for k in 0..16 {
        regions.push(Region { blocks: [k, k + 1, k + 2], total: 0 })
    }
    parallel for r in mut regions {
        for b in mut r.blocks {
            b = b * 3 + 1
            r.total += i64(b)
        }
    }
    parallel for r in regions {
        if r.total > 130 {
            println("region with {r.total}")    // in the order of the list
        }
    }
    var sum: i64 = 0
    for r in regions {
        sum += r.total
    }
    println("{sum}")
    // out: region with 138
    // out: region with 147
    // out: 1272
}
```

Nothing in the body reaches the list except through the element, so no iteration can reach another's. Without `mut`, each iteration reads its element, and the body can read the list as well, since no iteration changes it. Elements go to the threads in chunks, as a range's numbers do. Deques, maps and sets go through a range of indexes, or a list made from them.

## What an iteration can touch

An iteration changes only its own elements: places it reaches by indexing with the loop variable itself, such as `regions[k]` or `world.states[k].entities`. No other iteration has the same `k`, so no other iteration can reach them. It can read anything that no iteration changes.

| In the body | Allowed? |
| --- | --- |
| Change `xs[k]`, or any part of it | Yes |
| Change `x`, or any part of it, in `parallel for x in mut xs` | Yes |
| Use `xs` itself in `parallel for x in mut xs` | No: only through `x` |
| Change `w.blocks[k]` and `w.states[k]` in one iteration | Yes: separate fields are separate values |
| Change `grid[i][k]` in a loop over `k`, where `i` is the same in every iteration | Yes |
| Read `xs[k]`, or `ys[j]` for any `j`, when no iteration changes `ys` | Yes |
| Read `xs.len()` while iterations change elements of `xs` | Yes: changing an element doesn't change the length |
| Read `w.blocks` while iterations change `w.states[k]` | Yes: a different field |
| Change a local of its own, declared in the body | Yes |
| Change a variable from outside, such as `total += 1` | No: every iteration would change it at once |
| Change `xs[k + 1]`, `xs[j]` or `xs[0]` | No: another iteration might reach the same element |
| Read `xs[0]`, or `xs` as a whole, while iterations change `xs[k]` | No: another iteration might be changing it |
| `xs.push(x)` on a list from outside | No: that changes the list itself |

Calls count too. Passing `mut regions[k]` to a function lets it change that element and nothing else, and passing `w.blocks` lets it read them. The check is the same one that keeps a `mut` argument apart from the other arguments of a call ([functions](functions.md#parameter-modes)), applied between iterations.

```calx
fn main() {
    var xs: List<i32> = [1, 2, 3, 4]
    var total = 0
    parallel for k in 0usize..xs.len() {
        xs[k] *= 2
        total += xs[k]    // error: `total` is shared by every iteration of the `parallel for`
    }
    println("{total}")
}
```

To combine what the iterations found, have each write its own element, then combine them after the loop:

```calx
fn main() {
    let xs: List<i64> = [1, 2, 3, 4]
    var squares = [0i64; 4]
    parallel for k in 0usize..xs.len() {
        squares[k] = xs[k] * xs[k]
    }
    var total: i64 = 0
    for s in squares {
        total += s
    }
    println("{total}")    // out: 30
}
```

## Control flow

`continue` ends one iteration early. `break` and `return` aren't allowed in the body, since every iteration runs. A `break` or `return` inside a loop or function in the body is fine.

```calx
fn main() {
    var marks = [0; 8]
    parallel for k in 0usize..8 {
        if k % 2 == 1 {
            continue
        }
        marks[k] = 1
    }
    println("{marks[0]}{marks[1]}{marks[2]}")    // out: 101
}
```

## Loops inside loops

A `parallel for` can run inside another, even through a function the outer one calls, as a region running a loop of its own. A thread that has nothing left of the outer loop to take helps the inner loops instead, so a world with one busy region still gets every thread:

```calx
struct Region {
    cells: List<i64>
}

fn light(r: mut Region) {
    parallel for i in 0usize..r.cells.len() {
        r.cells[i] = r.cells[i] * 3 + 1
    }
}

fn main() {
    var regions: List<Region> = [Region { cells: [1, 2, 3] }, Region { cells: [4, 5] }]
    parallel for k in 0usize..regions.len() {
        light(mut regions[k])
    }
    println("{regions[0].cells} {regions[1].cells}")    // out: [4, 7, 10] [13, 16]
}
```

What the inner iterations print goes into the outer iteration that ran them, in order, and a panic in one is a panic in that outer iteration. So the output is still the same as if every loop ran one iteration after another.

One region whose loop runs a 200-step xorshift over 200,000 cells, 50 times, in ms:

| | 1 thread | 24 threads |
| --- | --- | --- |
| 1 region | 2,079 | 118 |
| 3 regions | 6,260 | 323 |

Before inner loops could use the pool, one region took 2,079 ms with any number of threads, and three took about as long, on three threads.

## The same result with any number of threads

Iterations can't see each other's changes, so what each computes doesn't depend on which thread ran it or when. Two more things make the whole program's behavior the same with one thread and with many:

- **Output comes out in order.** What an iteration prints with `print`, `println`, `eprint` or `eprintln` is held until the loop ends, then written in the order of the iterations. A program prints exactly what it would print running the iterations one after another.
- **A panic reports the first iteration that panicked.** If iterations panic, the program prints the output of every iteration before the first one that panicked, then that iteration's output and its panic, and exits with status 101, as running them in order would. Iterations after it may have run, but their output is dropped. Its [stack trace](printing.md#stack-traces) is the one running them in order would write too, from the body out through the loop to `main`.

```calx
fn main() {
    parallel for v in -2i32..2 {
        println("v {v}")
    }
    // out: v -2
    // out: v -1
    // out: v 0
    // out: v 1
}
```

Reading standard input inside a `parallel for` panics, since which iteration got which bytes would depend on timing.

## Actors

A `parallel for` over regions that send each other messages is how Calx runs actors. Each region keeps an outbox, which only its own iteration can reach, and the barrier after the loop delivers every message in a fixed order ([std.mail](../std/mail.md)).

## The threads

By default, a `parallel for` runs on as many threads as there are logical processors the program may run on, counting the thread that reaches the loop. [`std.pool.set_workers(n)`](../std/pool.md) changes that.

The threads are started by the first `parallel for` that needs them, and then wait for the next loop: they spin for a fifth of a millisecond, since loops often come one after another, then sleep until one starts. A loop's iterations are split into up to 8 chunks per thread, which the threads take in order, so a slow chunk doesn't hold the others up. A loop with one iteration, or with one thread to run it, runs on the thread that reaches it, and the loops inside it still use the pool.

Up to 64 inner loops can be open at once, each in a slot of its own that threads with nothing to do look at. A thread that has looked for a fifth of a millisecond and found nothing sleeps until an inner loop opens or the outer loop's last chunk finishes. With every slot taken, an inner loop runs on the thread that reaches it.

## In C

The body becomes a C function of its own, which the runtime calls for each chunk of the range, `[begin, end)`. The locals the body uses from outside reach it through a context struct. Numbers are copied in, since the body can't change them. Everything else is a pointer, which the function declares `restrict`, since the checker made sure no iteration reaches what another changes, and asserts whole and live with `CX_VIEW`, as for [view parameters](ownership.md#in-c). So clang optimizes the body as it would a function's.

```c
struct cx_par_game__main_3_c { cx_list_game__Region* regions; cx_i32 scale; cx_u64 cx__lo; };
static void cx_par_game__main_3(void* cx__v, cx_u64 cx__begin, cx_u64 cx__end) {
    struct cx_par_game__main_3_c* cx__c = (struct cx_par_game__main_3_c*)cx__v;
    cx_list_game__Region* restrict regions = cx__c->regions;
    CX_VIEW(regions);
    const cx_i32 scale = cx__c->scale;
    for (cx_u64 cx__i = cx__begin; cx__i < cx__end; cx__i++) {
        cx_usize k = (cx_usize)(cx__c->cx__lo + cx__i);
        game__tick(&(*regions).ptr[cx_index(k, (*regions).len, "game.calx:20:26")], scale);
        cx_cont3:;
    }
}
```

and in `main`, where the loop was:

```c
struct cx_par_game__main_3_c cx_t10 = {&regions, scale, (cx_u64)cx_t8};
cx_parallel((cx_u64)cx_t9 - (cx_u64)cx_t8, cx_par_game__main_3, &cx_t10);
```

The loop works out the range once, `cx_t8` to `cx_t9`, and if it isn't empty calls `cx_parallel(count, body, &context)` in the runtime, [runtime/calx_rt.c](../../runtime/calx_rt.c), which returns when every chunk has run.

A loop over elements passes the list's memory in the context instead, as `cx__at`, and the count is the list's length. Each iteration's view is a pointer to its element:

```c
cx_i64* restrict cx__at = cx__c->cx__at;
for (cx_u64 cx__i = cx__begin; cx__i < cx__end; cx__i++) {
    cx_i64* x = &cx__at[cx__i];
    (*x) = ((cx_i64)((cx_u64)(*x) + (cx_u64)((cx_i64)1LL)));
}
```

Still to build, in the [spec](../CALX_SPEC.md#parallel-loops): choosing a pool with `parallel(pool) for`.
