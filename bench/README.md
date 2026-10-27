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

## Results

| File | What it is |
| --- | --- |
| `results/rust-windows.json` | The Rust reference on Windows 11 |
| `results/rust-linux.json` | The Rust reference on Ubuntu under WSL2, on the same machine |

Each file names the commit, the machine (CPU, cores, memory, OS), the toolchain (rustc and its LLVM), and every run's mean tick time. The numbers in the gate are the medians.

The Rust reference as recorded, with Rust 1.91.1 (LLVM 21.1.2), 10 runs of 200 warmup and 1,000 measured ticks each:

| Platform | 1 worker | 24 workers | Speedup |
| --- | --- | --- | --- |
| Windows 11 | 25.75 ms per tick (p99 28.26) | 3.41 ms (p99 3.67) | 7.5× |
| Linux (WSL2) | 26.04 ms (p99 28.67) | 3.52 ms (p99 4.73) | 7.4× |

Every run printed checksum `0a7e97a835579c25`, at both worker counts. The speedup is well under 24 because 16 of the cores are efficiency cores, and the barrier step (delivery and the checksum) runs on one thread.

The machine is an Intel Core i9-13900K: 8 performance cores and 16 efficiency cores, so 24 physical cores and 32 threads. Both platforms run 1 and 24 workers. WSL2 reports the CPU as 16 cores with 2 threads each, which isn't the real topology, so the Linux runs use the Windows count.
