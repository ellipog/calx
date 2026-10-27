---
title: Benchmark files
description: How to build, run and record the tick-loop benchmark, and where its results live.
---

# Benchmark files

The tick-loop benchmark behind the Phase 3 performance gate. [`docs/benchmarks.md`](../docs/benchmarks.md) defines the workload, how it's measured and the gate. This folder holds the implementations, the runner and the recorded results.

```sh
py -I bench/run.py rust                 # 10 runs each at 1 worker and one per physical core
py -I bench/run.py rust --record        # the same, written to bench/results/rust-<platform>.json
py -I bench/run.py rust --threads 1,8 --runs 3 --warmup 20 --ticks 100    # a quick look
py -I bench/run.py calx                 # the Calx version, built with build/compiler/<platform>/calx
py -I bench/build.py --record           # how fast Calx builds, written to bench/results/build-<platform>.json
```

On Linux, use `python3` for `py`. The runner builds the version first, into `build/bench/<platform>/`. Every run of the full 200 + 1,000 ticks must print the checksum in [`expected.json`](expected.json), and every run of a configuration must agree, or the runner stops. Only full runs can be recorded.

## The Rust reference (`P3.1`)

[`rust/`](rust/) is idiomatic, safe Rust with only the standard library: no `unsafe`, bounds checks left in, and wrapping arithmetic where Java wraps. It builds with the release profile `docs/benchmarks.md` sets: thin LTO, one codegen unit, `panic = "abort"`, and `x86-64-v3`. Run it directly with:

```sh
tickbench [--threads N] [--warmup 200] [--ticks 1000] [--stats]
```

It prints one line of JSON. `--stats` also prints the palette layouts and scheduled-tick count to stderr at the start, after the warmup and at the end.

Where `docs/benchmarks.md` lets an implementation choose, the reference uses:

- **Threads.** Persistent workers that take regions from a shared counter, with a spinning barrier between passes, so no thread starts or sleeps inside a tick. With one worker, everything runs on the main thread.
- **The fast native map.** The standard `HashMap` (a SwissTable) with an Fx-style multiply hasher, which is about as fast as a general map gets in Rust.
- **The Java-compatible map.** Written by hand, as the doc requires, with nodes in a vector and a free list.

## The Calx version (`P3.2`)

[`calx/tickbench.calx`](calx/tickbench.calx) is the same program in Calx, with the same algorithms and the same checksum. It's safe Calx: no `unsafe` except reading the clock, and bounds checks left in. The runner builds it with `calx --release`. It takes the same flags as the Rust reference.

Each pass is a `parallel for` over the regions, on the runtime's pool, and `--threads` sets the pool's size with `std.pool.set_workers`. That's the shape of the Rust reference: persistent threads that take regions in order from a shared counter, with the barrier step on the main thread. Each region's messages and handoffs go into its own outbox, which the barrier delivers in sender order, since actors aren't built yet. The checker proves the passes race-free: an iteration changes only `world.blocks[k]` and `world.states[k]`, and the entity pass reads every region's blocks while changing only its own state.

## Results

| File | What it is |
| --- | --- |
| `results/rust-windows.json` | The Rust reference on Windows 11 |
| `results/rust-linux.json` | The Rust reference on Ubuntu under WSL2, on the same machine |
| `results/calx-windows.json` | The Calx version on Windows 11 |
| `results/calx-linux.json` | The Calx version on Ubuntu under WSL2 |
| `results/build-windows.json`, `results/build-linux.json` | How fast Calx builds, from [`build.py`](build.py) |

Each file names the commit, the machine (CPU, cores, memory, OS), the toolchain (rustc and its LLVM), and every run's mean tick time. The numbers in the gate are the medians.

The Rust reference as recorded, with Rust 1.91.1 (LLVM 21.1.2), 10 runs of 200 warmup and 1,000 measured ticks each:

| Platform | 1 worker | 24 workers | Speedup |
| --- | --- | --- | --- |
| Windows 11 | 25.75 ms per tick (p99 28.26) | 3.41 ms (p99 3.67) | 7.5× |
| Linux (WSL2) | 26.04 ms (p99 28.67) | 3.52 ms (p99 4.73) | 7.4× |

Every run printed checksum `0a7e97a835579c25`, at both worker counts. The speedup is well under 24 because 16 of the cores are efficiency cores, and the barrier step (delivery and the checksum) runs on one thread.

The machine is an Intel Core i9-13900K: 8 performance cores and 16 efficiency cores, so 24 physical cores and 32 threads. Both platforms run 1 and 24 workers. WSL2 reports the CPU as 16 cores with 2 threads each, which isn't the real topology, so the Linux runs use the Windows count.

The Calx version as recorded, with clang 23.1.4, the same runs:

| Platform | 1 worker | Against Rust | 24 workers | Against Rust |
| --- | --- | --- | --- | --- |
| Windows 11 | 18.06 ms per tick (p99 25.35) | 0.70× | 2.25 ms (p99 2.55) | 0.66× |
| Linux (WSL2) | 18.01 ms (p99 20.57) | 0.69× | 2.46 ms (p99 2.94) | 0.70× |

Every run printed the same checksum as the Rust reference. D89 lists the compiler changes behind the lead: `-O3`, telling clang that views are always valid, and letting `mut` arguments be separate fields of one value. D90 is `parallel for` and its pool. The Windows 1-worker p99 is high because of other load on the machine during that run. An earlier one-worker run on a quieter machine gave 17.56 ms with p99 20.03.

## Build times (D64)

[`build.py`](build.py) times the compiler the way G3.4 measures it. It generates a program of 64 modules, each importing the one before, about 9,900 lines in all, and builds it. It also builds a one-line program and the tick benchmark. Each time is the median of 5, from a fresh process.

| Measure | Windows 11 | Linux (WSL2) | G3.4 |
| --- | --- | --- | --- |
| `calx check`, 64 modules | 32 ms | 24 ms | |
| Debug build, empty cache, 64 modules | 520 ms | 278 ms | |
| Release build, empty cache, 64 modules | 663 ms | 550 ms | |
| Debug build after changing one of the 64 | 123 ms | 78 ms | |
| `calx run` of a small program, empty cache | 274 ms | 302 ms | under 500 ms |
| `calx run` of it again, nothing changed | 16 ms | 21 ms | under 50 ms |
| Debug build of the tick benchmark after changing it | 125 ms | 121 ms | under 1 s |

An empty cache means the runtime is built too. The generated program goes in the temp folder, so under WSL it's on Linux's own file system: through `/mnt/c`, reading the 65 files took most of a 420 ms check.

CI runs both scripts on every push, on GitHub's shared runners. There the times are only a rough guide, but the tick benchmark's checksum must still match.

## Math functions

[`math/math.calx`](math/math.calx) times `std.math`'s `exp`, `log`, `pow`, `sin`, `cos`, `tan`, `atan2` and `acos` on 2 million random arguments each, best of 7, and [`math/libm.c`](math/libm.c) times the C library's on the same arguments in the same loops. The C library is what Rust's `f64::exp`, `ln`, `powf` and the rest call: the Windows CRT, and glibc on Linux.

```sh
calx build --release -o math bench/math/math.calx
clang -O3 -march=x86-64-v3 -fno-builtin -o libm bench/math/libm.c -lm
```

Both print a sum of the results, which must agree to the last digit or two. Each Calx loop passes its sum through [`std.hint.black_box`](../docs/std/hint.md) before the clock is read again: release builds inline `std.math` into the loop (D99), and clang could otherwise move a loop with no side effects past the clock. `libm.c` has an empty `asm` in the same place, since clang otherwise sees that only the last of the 7 sums is used and drops the others' additions. The times, on the reference machine (i9-13900K, pinned to one P-core), are in [std.math](../docs/std/math.md#speed).
