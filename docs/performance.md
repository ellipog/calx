---
title: Performance
description: How fast Calx programs run and build, against C, Rust, Go and Python, on Windows and Linux, and where each set of benchmarks is.
---

# Performance

Calx is timed three ways, each on its own page, and on the same machine, an Intel Core i9-13900K, under Windows 11 and under Ubuntu in WSL2. On [calxlang.com/performance](https://calxlang.com/performance/) the results are drawn from the recorded runs, with each language's wins and losses on each system.

These are our benchmarks, and we wrote every version ourselves, so they say how Calx does on work shaped like these. Every recorded run is in [`bench/results`](../bench/results), and the programs that made them are beside it in [`bench`](../bench).

## The benchmarks

- **[Language benchmarks](language-benchmarks.md)** time Calx against C, Rust, Go and Python on 25 programs: the Benchmarks Game's classic compute, programs shaped like real work, such as JSON, CSV, sorting, hash maps and compression, and parallel loops on every processor. They also time how long each language takes to build a program, from scratch and after an edit, and to start one. Start here.
- **[Scripting benchmarks](scripting-benchmarks.md)** time what scripts do, taking text apart, writing numbers, JSON, walking a tree of files and starting up, in Calx, Rust and Python, with the CPU time and memory each used.
- **[Tick-loop benchmark](benchmarks.md)** is a fixed simulation, the shape of a game server's tick, written in Calx and in Rust with the same algorithms. CI runs it on every push and checks its checksum.

## Where Calx is slower

Each benchmark page says where Calx loses and why, and the board has a card for each loss Calx can do something about. The language benchmarks' [summary](language-benchmarks.md#what-they-show) lists them all.

## Running them

[`bench/README.md`](../bench/README.md) says how to run each set, what it needs installed, and which file in `bench/results` it records to.
