---
title: Benchmark files
description: How to build, run and record the tick-loop benchmark, and where its results live.
---

# Benchmark files

The tick-loop benchmark behind the Phase 3 performance gate. [`docs/benchmarks.md`](../docs/benchmarks.md) defines the workload, how it's measured and the gate. This folder holds the implementations, the runner and the recorded results.

```sh
calx run bench/run.calx -- rust             # 10 runs each at 1 worker and one per physical core
calx run bench/run.calx -- rust --record    # the same, written to bench/results/rust-<os>.json
calx run bench/run.calx -- rust --threads 1,8 --runs 3 --warmup 20 --ticks 100    # a quick look
calx run bench/run.calx -- calx             # the Calx version, built by the calx that runs this
calx run bench/build.calx -- --record       # how fast that calx builds, written to bench/results/build-<os>.json
```

`calx` there is the compiler to measure, such as `build/compiler/calx`. The runner builds the version first, into `build/bench/<os>/`. Every run of the full 200 + 1,000 ticks must print the checksum in [`expected.json`](expected.json), and every run of a configuration must agree, or the runner stops. Only full runs can be recorded.

## The Rust reference (`P3.1`)

[`rust/`](rust/) is idiomatic, safe Rust with only the standard library: no `unsafe`, bounds checks left in, and wrapping arithmetic where Java wraps. It builds with the release profile `docs/benchmarks.md` sets: thin LTO, one codegen unit, `panic = "abort"`, and `x86-64-v3`. Run it directly with:

```sh
tickbench [--threads N] [--warmup 200] [--ticks 1000] [--stats]
```

It prints one line of JSON. `--stats` also prints the palette layouts and scheduled-tick count to stderr at the start, after the warmup and at the end.

Where `docs/benchmarks.md` lets an implementation choose, the reference uses:

- **Threads.** Persistent workers that take regions from a shared counter, with a spinning barrier between passes, so no thread starts or sleeps inside a tick. With one worker, everything runs on the main thread.
- **The fast native map.** The standard `HashMap` (a SwissTable) with an Fx-style multiply hasher, which is about as fast as a general map gets in Rust.
- **The Java-compatible map.** Written by hand, as the doc requires, with nodes in a vector and a free list, since Rust has none.

## The Calx version (`P3.2`)

[`calx/tickbench.calx`](calx/tickbench.calx) is the same program in Calx, with the same algorithms and the same checksum. Its Java-compatible map is Calx's own [`JavaHashMap<i64, i32>`](../docs/reference/java-maps.md) (D105), which keeps the rules the Rust one is written from; the workload never makes a tree bin. It's safe Calx: no `unsafe` except reading the clock, and bounds checks left in. The runner builds it with `calx --release`. It takes the same flags as the Rust reference.

Each pass is a `parallel for` over the regions, on the runtime's pool, and `--threads` sets the pool's size with `std.pool.set_workers`. That's the shape of the Rust reference: persistent threads that take regions in order from a shared counter, with the barrier step on the main thread. Each region's messages and handoffs go into its own outbox, which the barrier delivers in sender order, since actors aren't built yet. The checker proves the passes race-free: an iteration changes only `world.blocks[k]` and `world.states[k]`, and the entity pass reads every region's blocks while changing only its own state.

## Results

| File | What it is |
| --- | --- |
| `results/rust-windows.json` | The Rust reference on Windows 11 |
| `results/rust-linux.json` | The Rust reference on Ubuntu under WSL2, on the same machine |
| `results/calx-windows.json` | The Calx version on Windows 11 |
| `results/calx-linux.json` | The Calx version on Ubuntu under WSL2 |
| `results/build-windows.json`, `results/build-linux.json` | How fast Calx builds, from [`build.calx`](build.calx) |
| `results/script-windows.json`, `results/script-linux.json` | Calx against Rust and Python as scripting languages, from [`script.calx`](script.calx) |
| `results/suite-windows.json`, `results/suite-linux.json` | Calx against C, Rust, Go and Python on 25 programs, from [`suite.calx`](suite.calx) |

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

[`build.calx`](build.calx) times the compiler the way G3.4 measures it. It generates a program of 64 modules, each importing the one before, about 9,900 lines in all, and builds it. It also builds a one-line program and the tick benchmark. Each time is the median of 5, from a fresh process.

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

## Hashes, checksums and AES

[`digest/digest.calx`](digest/digest.calx) times MD5, SHA-1, SHA-256, SHA-512, CRC-32, Adler-32, CRC-64 and AES-CFB8 both ways, in MiB/s over 64 MiB, the best of 5, and [`digest/digest.py`](digest/digest.py) the same hashes and checksums with Python's `hashlib`, which is OpenSSL's, and `zlib`. Python has no CRC-64 or AES. These are the runtime's paths with x86's SHA-NI, AES-NI, PCLMUL and AVX2, and with Arm64's SHA, AES, CRC32, PMULL and NEON instructions (CLX-281).

```sh
calx run --release bench/digest/digest.calx
python -I bench/digest/digest.py
```

The [bench workflow](../.github/workflows/bench.yml) runs both on GitHub's Arm and x64 runners with the newest nightly, installed as a user installs it: `gh workflow run bench`. With `-f checkout=true`, it also builds them with the nightly's compiler and the pushed commit's runtime and std, and times the two in turn on each runner, so a change to the runtime's C is measured on Arm before a nightly has it. Their `total` lines must match. The speed sections of [std.hash](../docs/std/hash.md#speed), [std.crypto](../docs/std/crypto.md) and [std.zlib](../docs/std/zlib.md) give its numbers, with the machine each is from.

## Pushing onto a list

[`push/push.calx`](push/push.calx) pushes a million values onto a list three ways: 4 bytes each, big-endian, as a packet encoder writes an `int`; one byte each, as it writes a VarInt below 128; and an `i64` each, whose type is the list's length's. Each loop runs once onto a new list in a local, which it returns, and twice onto a list it's given to change, which the caller keeps and clears, so no time goes to the allocator: once called directly, so clang can put it inside the caller, and once through a function value it can't see through (`std.hint.black_box`), as a function called from many places is kept apart. A last loop writes each value as an int and then as a VarInt through a writer's methods, a struct that holds the list, as Calcite's packets do. Each time is the best of 50. [`push/push.rs`](push/push.rs) is the same loops with `Vec`, built as the tick benchmark's Rust reference is.

```sh
calx build --release -o push bench/push/push.calx
rustc -C opt-level=3 -C codegen-units=1 -C panic=abort -C target-cpu=x86-64-v3 -o push_rs bench/push/push.rs
```

Both print the same total. The times are in [Lists](../docs/reference/lists.md#in-c).

## Building strings

[`strings/strings.calx`](strings/strings.calx) builds strings as a server builds chat, JSON and log lines: a million ASCII runes, a million runes of 1 to 4 bytes mixed, and a million words of 3 to 7 bytes, each onto a new string in a local; then the ASCII runes again, and a text escaped as JSON a rune at a time, onto a string it's given to change, through a function value clang can't see through. Each time is the best of 50. [`strings/strings.rs`](strings/strings.rs) is the same loops with Rust's `String`.

```sh
calx build --release -o strings bench/strings/strings.calx
rustc -C opt-level=3 -C codegen-units=1 -C panic=abort -C target-cpu=x86-64-v3 -o strings_rs bench/strings/strings.rs
```

Both print the same total. The times are in [Strings](../docs/reference/strings.md#in-c).

## Taking text apart

[`text/text.calx`](text/text.calx) takes a text of 200,000 CSV lines apart as a script does: the fields of every line, trimmed, in loops over `lines()` and `split(",")`; the lines joined with `|`; and that text with a word replaced. Each time is the best of 7. [`text/text.rs`](text/text.rs) is the same with Rust's `str` methods, and [`text/text.py`](text/text.py) with Python's.

```sh
calx build --release -o text bench/text/text.calx
rustc -C opt-level=3 -C codegen-units=1 -C panic=abort -C target-cpu=x86-64-v3 -o text_rs bench/text/text.rs
py -I bench/text/text.py
```

All three print the same total. The times are in [Strings](../docs/reference/strings.md#in-c).

## Reading and writing JSON

[`json/json.calx`](json/json.calx) reads and writes 100,000 player records as a script does with [`std.json`](../docs/std/json.md): the text parsed into a tree, the tree written back as compact text, and the text read into `@json` structs. Each time is the best of 7. [`json/rust`](json/rust) is the same with serde_json, its `Value` and `#[derive(Deserialize)]`, and [`json/json.py`](json/json.py) with Python's `json` module and dataclasses.

```sh
calx build --release -o json bench/json/json.calx
cargo build --release --manifest-path bench/json/rust/Cargo.toml
py -I bench/json/json.py
```

The times are in [Scripting benchmarks](../docs/scripting-benchmarks.md).

## Calx as a scripting language

[`script.calx`](script.calx) times Calx against Rust and Python on what scripts do. Each benchmark is a program in each language, with the same algorithms, written as that language usually writes it:

| Folder | What it does |
| --- | --- |
| [`text/`](text/) | 200,000 CSV lines taken apart, joined and a word replaced, as above |
| [`format/`](format/) | Numbers written as text: doubles to three places, integers padded, integers and doubles in their shortest digits |
| [`json/`](json/) | 100,000 records parsed, written and read into structs, as above |
| [`walk/`](walk/) | A tree of 10,000 files in 420 directories walked, and `**/*.calx` globbed, with the walkdir and glob crates in Rust |
| [`start/`](start/) | The smallest program, which prints a line, from outside: started, and as a script changed and run again |

```sh
calx run bench/script.calx                        # 5 rounds of each, the tables in Markdown
calx run bench/script.calx -- --only json,walk    # some of them
calx run bench/script.calx -- --record            # written to bench/results/script-<os>.json
calx run bench/script.calx -- --only start --record    # start recorded again, the others kept
```

A record gives each benchmark's results the time and the commit they were recorded at. With `--only`, the benchmarks the run leaves out keep their results from the file.

Each program is run as a fresh process, the languages taking turns, in the other order every other round. A program times its own steps, the best of 7, and prints them with a total its work came to, which must be the same in all three languages, or the runner stops. The tables give each step's best over the rounds, and for the whole process its CPU time and the most memory it held, the median over the rounds, from [`std.process`](../docs/std/process.md#what-it-used). It builds the Calx programs with `calx build --release`, the Rust ones with `-C opt-level=3`, one codegen unit, `panic = "abort"` and `x86-64-v3`, and thin LTO for the two with crates, and runs Python's own program, not the `py` launcher. Everything it makes goes in a folder of its own in the temp folder, removed at the end.

It doesn't pin itself to cores. On a processor with two kinds, start it pinned, and the programs it runs are too: on Windows, `start /affinity 5554 /b /wait calx run bench/script.calx` puts it on the i9-13900K's performance cores, and on Linux `taskset -c 4` on one of them.

## Against C, Rust, Go and Python

[`suite.calx`](suite.calx) times Calx against C, Rust, Go and Python on 25 programs, each written in each language with the same algorithm, in [`suite/`](suite/), one folder a benchmark. [Language benchmarks](../docs/language-benchmarks.md) has the results and how they're made.

| Group | Benchmarks |
| --- | --- |
| compute | The Benchmarks Game's [`nbody`](suite/nbody/), [`spectralnorm`](suite/spectralnorm/), [`fannkuch`](suite/fannkuch/), [`binarytrees`](suite/binarytrees/), [`mandelbrot`](suite/mandelbrot/), [`fasta`](suite/fasta/), [`knucleotide`](suite/knucleotide/), [`revcomp`](suite/revcomp/), [`regexredux`](suite/regexredux/) and [`pidigits`](suite/pidigits/) |
| realistic | [`json`](suite/json/), a [`csv`](suite/csv/) report, [`logs`](suite/logs/) analysis, [`wordcount`](suite/wordcount/), [`sort`](suite/sort/), [`hashmap`](suite/hashmap/), many small [`objects`](suite/objects/), building and splitting [`text`](suite/text/), many [`files`](suite/files/), [`compress`](suite/compress/) (gzip, zstd, xz), [`hash`](suite/hash/) (SHA-256, CRC-32) and [`png`](suite/png/) |
| parallel | [`parmandel`](suite/parmandel/), [`parspectral`](suite/parspectral/) and [`parsum`](suite/parsum/): `parallel for` against Rayon, goroutines and OpenMP |
| dx | A hello program and a generated 64-module one ([`dx.calx`](dx.calx)) built from scratch, after an edit and unchanged; their sizes; and starting hello |

```sh
calx run --release bench/suite.calx                            # everything, 5 rounds
calx run --release bench/suite.calx -- --groups compute         # one group
calx run --release bench/suite.calx -- --only json,csv --langs calx,rust
calx run --release bench/suite.calx -- --quick                  # small inputs, one round: does everything build and agree?
calx run --release bench/suite.calx -- --record                 # written to bench/results/suite-<os>.json
calx run --release bench/suite.calx -- --tables                 # the docs page's tables and tallies, from the records
```

It needs `clang`, `cargo`, `go`, Python 3 (`py` on Windows, `python3` elsewhere) and, for the dx group, `ninja` on the PATH, and for the parallel C programs LLVM's OpenMP runtime: the LLVM installer has it on Windows, and on Linux it's a package such as `libomp-dev`, or apt.llvm.org's `libomp-23-dev` for its clang 23. clang's own includes and libraries are searched, and `C_INCLUDE_PATH`, `LIBRARY_PATH` and `LD_LIBRARY_PATH` add to them. A language it can't build is left out of that benchmark, and the run says why. Run it with `--release`: it pins itself on Windows through the system's own functions, and only a release build leaves those out elsewhere, since it sees they're never called.

- **Builds.** Calx with `calx build --release`; C with clang at the flags Calx's release builds use, `-O3 -march=x86-64-v3 -ffp-contract=off -fno-math-errno -flto=thin`, and `-fopenmp` for the parallel ones; Rust with the release profile in [`suite/rust/Cargo.toml`](suite/rust/Cargo.toml) (opt-level 3, thin LTO, one codegen unit, `panic = "abort"`) and `-C target-cpu=x86-64-v3`; Go with `go build`, `GOAMD64=v3` and cgo off, from [`suite/go.mod`](suite/go.mod); Python as it is, with `-I`.
- **Inputs.** [`inputs.calx`](inputs.calx) writes the inputs from fixed seeds: fasta's output, JSON records, a CSV of sales, an access log, words, numbers, an image. Each benchmark's arguments name the ones it reads.
- **Checking.** Each program prints what its work came to, and every language must print the same, byte for byte, or that language is left out of the benchmark and the run says so. Lines that start with `info ` may differ between languages, such as a compressor's output size or a parallel sum's last bits. The record keeps them, and says when a program's changed from one run to the next, as a parallel float sum's can.
- **Timing.** One run to warm up and check, then `--rounds` timed runs, the languages taking turns, in the other order every other round; a program that took over 10 s to warm up runs 3. Each run is a fresh process, timed from outside, as GNU `time` times it: its wall time from just before it starts until it exits, the CPU time of all its threads, and its peak memory, from [`std.process`'s `Usage`](../docs/std/process.md#what-it-used). The tables give medians, and the record has every run. Each program starts [`measured`](../docs/std/process.md#a-peak-of-its-own-on-linux): on Linux a child's peak resident set otherwise starts at the peak of the process that started it, which for the runner, holding inputs and outputs, would be hundreds of megabytes. Measured, a program that does nothing peaks at about 2 MB, and the record says how much.
- **A shared machine.** Each run also measures how busy the rest of the machine was, as other programs' share of all the processors. Over 10%, the run is repeated, up to twice, and the quietest kept; the record says how busy each run's machine was. Builds look for a quiet quarter of a second before they start.
- **Pinning.** Single-threaded programs run on the i9-13900K's performance cores, one thread on each of 7, out of power throttling on Windows. A parallel benchmark at up to 7 threads gets those cores, at 8 all 8, and above that every processor. The runner pins itself with std.process's `set_affinity`, and the programs it starts take that with them.
- **Under WSL**, run it with a compiler in the Linux file system, as an installed Calx is: copy `build/compiler/linux/calx` to a `bin/` folder there, with `runtime/` and `std/` beside that `bin/`, and run that copy. One on the Windows drive is read through WSL's file share, which adds about 6 ms to starting it and about 1 ms to each of its own files a build checks: a no-op build of the dx group's program took 15 to 19 ms that way, and sometimes a few hundred, against 1.0 to 1.6 ms from a copy (CLX-302). The suite says so when it's run from `/mnt/`.

A record names the commit it measured, and lists any of the suite's files that differed from it. With `--only` or `--groups`, the benchmarks the run leaves out keep their results from the file, and with `--langs`, the languages it leaves out keep theirs, each marked with the commit and time it was recorded at.
