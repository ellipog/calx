---
title: Language benchmarks
description: Calx timed against C, Rust, Go and Python on 25 programs, from the Benchmarks Game to JSON, CSV, compression and parallel loops, and on builds and startup.
---

# Language benchmarks

Calx timed against the languages people choose between for the same work: C, Rust, Go and Python. There are 25 programs in three groups, the Benchmarks Game's classic compute, programs shaped like real work, and parallel loops, plus how long each language takes to build a program and to start one. Each program is written in each language with the same algorithm, as that language usually writes it, and prints what its work came to. Every language must print the same, byte for byte, or its result isn't counted. The programs are in [`bench/suite`](../bench/suite), one folder a benchmark, with the runner, [`bench/suite.calx`](../bench/suite.calx), and every recorded run in [`bench/results`](../bench/results).

These are our benchmarks, and we wrote every version ourselves, so they say how Calx does on work shaped like these. If you can make one of the C, Rust, Go or Python programs faster without changing what it computes, please send a pull request: the numbers will change with it.

## What they show

How many programs Calx took less time on than each language, how many came within 5% either way, and how many took it longer, with the parallel loops counted on every processor:

| Calx against | Programs both have | Windows 11 | Ubuntu 26.04 in WSL2 |
| --- | --- | --- | --- |
| C | 19 | 15 faster, 4 even | 12 faster, 7 even |
| Rust | 25 | 19 faster, 5 even, 1 slower | 17 faster, 6 even, 2 slower |
| Go | 25 | 19 faster, 3 even, 3 slower | 17 faster, 4 even, 4 slower |
| Python | 21 | 21 faster | 21 faster |

- **Loops over numbers are even with C.** Calx compiles through C that clang builds at C's flags, so n-body, spectral-norm and mandelbrot come within about 1% of C, and within 9% of Rust either way.
- **Most of Calx's wins are its runtime and library.** Its allocator, hash maps, sort, `std.json`, `std.csv`, `std.regex`, `std.bigint` and `std.image.png` are written for these jobs. Binary-trees and many small objects take C and Rust 2 to 5 times as long on the Windows heap, and 1.3 to 4.3 times with glibc's on Linux. Hash maps take the others 1.9 to 2.8 times as long with their default hashes, and JSON takes serde_json 34 to 63% longer. Text parts a program only looks at, as word count's words and the log's fields, are views into the text, not copies, and a String key of 15 bytes or fewer is kept in its table's slot.
- **The losses, and why.**
  - n-body goes to Rust, by 7 to 9%, which is how LLVM's vectorizer packs the loop in each version.
  - Compression goes to Go, whose compressors write bigger files.
  - binary-trees goes to Go's collector, by 7 to 10%.
  - The parallel mandelbrot goes to Go, by 5 to 8% at 4 threads and up, about what Calx is behind it on one thread.
  - On Linux, on all 32 processors, the float sum goes to Rust and Go, though there the times swing by a fifth from one recording to the next.

  The notes under each table say more, and the board has a card for each loss Calx can do something about.
- **Calx often holds more memory.** In binary-trees, its allocator keeps freed blocks for the next ones of their size, its CSV reader reads the whole file into memory, and its float sort uses a second list the size of the first.
- **Only Calx's parallel float sum came out the same at every thread count**, to the last bit. A `parallel for` can't add to one total, so the program adds fixed chunks' sums in order.
- **Calx builds a program again faster than C, Rust or Go after a one-module edit**, and C builds a program from scratch faster, as Calx compiles its runtime the first time.

## How it's measured

- **Runs.** Each program runs once to warm up and to check its output, then 5 times on Windows and 3 on Linux, as a fresh process timed from outside, the languages taking turns, in the other order every other round. A program that took over 10 seconds to warm up runs 3 times, which only Python's do. The tables give the median wall time, and the most memory the process held at once, its peak working set on Windows and peak resident set on Linux, also the median. On Linux a small program starts each one, since a child's peak there starts at that of the process that started it; a program that does nothing peaks at about 2 MB that way. The record has every run's wall time, CPU time and memory.
- **A quiet machine.** The machine is shared, so each run also measures how busy the rest of it was: the CPU time every other program used, as a share of all the processors. A run where that was over 10% is run again, up to twice, and the quietest kept. Under WSL only Linux's own processes can be seen.
- **Pinned.** On an Intel Core i9-13900K, with 8 performance cores and 16 efficiency cores. Under Windows 11 every single-threaded program ran on 7 of the performance cores, one thread on each, out of power throttling. Under Ubuntu in WSL2 on the same machine, they were pinned the same way. The runner pins itself with `std.process`'s `set_affinity`, and the programs it starts take that with them.
- **Builds.** Calx with `calx build --release`. C with clang at the flags Calx's release builds use: `-O3 -march=x86-64-v3 -ffp-contract=off -fno-math-errno -flto=thin`. Rust with opt-level 3, thin LTO, one codegen unit, `panic = "abort"` and `-C target-cpu=x86-64-v3`. Go with `go build`, `GOAMD64=v3` and cgo off. Python 3 as it is, with `-I`. So every compiled language targets the same CPU level, with AVX2.
- **Libraries.** Each language uses its standard library where it has what a benchmark needs. Where it doesn't, Rust uses the crate Rust programs usually reach for (serde_json, csv, regex, num-bigint, flate2, zstd, xz2, sha2, crc32fast, png, Rayon) and Go its usual module for zstd and xz (klauspost/compress, ulikunitz/xz). C has no regular expressions, big integers, JSON, compression, hashes or images in its standard library, so it sits those out, and the C versions write their own hash maps. Python has no image formats, and its threads take turns under its lock, so it sits out PNG and the parallel loops.

| | Windows 11 | Ubuntu 26.04 in WSL2 |
| --- | --- | --- |
| Calx | 0.1.0-dev at 8feb83b, 2cf57a1, 997f6fb, bbb5ca7 and 8cf937c, through clang 23.1.4 | 0.1.0-dev at 8feb83b, 2cf57a1, 997f6fb, bbb5ca7 and 8cf937c, through clang 23.1.5 |
| C | clang 23.1.4 | clang 23.1.5 |
| OpenMP | LLVM's libomp 23.1.4 | LLVM's libomp 23.1.6 |
| Rust | 1.91.1, with LLVM 21.1.2 | 1.91.0 |
| Go | 1.27.3 | 1.27.3 |
| Python | 3.14.6 | 3.14.4 |
| Rounds | 5 | 3, and 5 for the builds and the programs timed since 2cf57a1 |

The programs that Calx's changes since the first recording touch, fifteen of them and Linux's builds, were timed again at 8feb83b in every language, but Go on Linux, which wasn't installed there then. Ten of them were timed again at 2cf57a1, after the changes for log analysis and reverse-complement (CLX-308, CLX-309): k-nucleotide, reverse-complement, regex-redux, JSON, CSV, log analysis, word count, sorting, hash maps and text, in every language but Go on Linux, log analysis and regex-redux again at 997f6fb, after a `var` String could hold a view (CLX-310), log analysis and text again at bbb5ca7, after a split list only indexed by number could be on the stack (CLX-314), and JSON, log analysis, word count and hash maps again at 8cf937c, after a String key of 15 bytes or fewer could be kept in its table's slot (CLX-316). The rest, and Go's results on Linux, are from the first recording, with Calx as of 9ebb972. Each benchmark's entry in the record has the commit it was timed at.

## Classic compute

The [Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)'s programs, each written as that site's plainer versions are: the same algorithm in every language, no SIMD by hand, no threads.

The median wall time and the most memory held, the fastest in bold. A dash is a language with no program for it.

On Windows 11:

| Benchmark | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| n-body, 10 million steps | 258 ms, 3.8 MB | 257 ms, 3.8 MB | **241 ms**, 4.1 MB | 507 ms, 5.5 MB | 31,197 ms, 11 MB |
| spectral-norm, n = 3,000 | **271 ms**, 3.9 MB | 272 ms, 3.8 MB | 273 ms, 4.1 MB | 274 ms, 5.7 MB | 32,559 ms, 11 MB |
| fannkuch-redux, n = 11 | **1,404 ms**, 3.8 MB | 1,589 ms, 3.7 MB | 1,596 ms, 4.1 MB | 1,546 ms, 5.5 MB | 41,513 ms, 11 MB |
| binary-trees, depth 18 | 819 ms, 68 MB | 1,644 ms, 36 MB | 1,724 ms, 36 MB | **758 ms**, 41 MB | 15,758 ms, 59 MB |
| mandelbrot, 4,000 by 4,000 | 625 ms, 5.8 MB | 627 ms, 5.2 MB | 643 ms, 5.6 MB | **609 ms**, 7.4 MB | 27,256 ms, 14 MB |
| fasta, n = 2.5 million | **198 ms**, 3.9 MB | 209 ms, 3.8 MB | 216 ms, 4.2 MB | 245 ms, 5.5 MB | 3,592 ms, 11 MB |
| k-nucleotide, fasta n = 1 million | **146 ms**, 28 MB | 168 ms, 22 MB | 341 ms, 26 MB | 403 ms, 42 MB | 3,496 ms, 29 MB |
| reverse-complement, fasta n = 1 million | **22.8 ms**, 19 MB | 29.8 ms, 18 MB | 27.6 ms, 19 MB | 32.0 ms, 26 MB | 79.0 ms, 32 MB |
| regex-redux, fasta n = 1 million | **103 ms**, 46 MB | — | 135 ms, 53 MB | 3,803 ms, 64 MB | 1,430 ms, 100 MB |
| pidigits, 10,000 digits | **581 ms**, 6.4 MB | — | 5,106 ms, 8.6 MB | 877 ms, 47 MB | 2,197 ms, 12 MB |

On Ubuntu 26.04 in WSL2:

| Benchmark | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| n-body, 10 million steps | 258 ms, 2.1 MB | 256 ms, 2.1 MB | **236 ms**, 2.2 MB | 507 ms, 2.1 MB | 21,062 ms, 10 MB |
| spectral-norm, n = 3,000 | 280 ms, 2.2 MB | **277 ms**, 2.1 MB | 281 ms, 2.2 MB | 278 ms, 2.1 MB | 22,015 ms, 10 MB |
| fannkuch-redux, n = 11 | 1,563 ms, 2.1 MB | 1,590 ms, 2.1 MB | 1,551 ms, 2.2 MB | **1,511 ms**, 2.1 MB | 28,085 ms, 9.7 MB |
| binary-trees, depth 18 | 785 ms, 66 MB | 1,014 ms, 34 MB | 1,115 ms, 34 MB | **709 ms**, 35 MB | 6,820 ms, 58 MB |
| mandelbrot, 4,000 by 4,000 | 628 ms, 4.1 MB | 631 ms, 4.1 MB | 642 ms, 3.9 MB | **608 ms**, 3.9 MB | 18,265 ms, 12 MB |
| fasta, n = 2.5 million | **202 ms**, 2.2 MB | 224 ms, 2.1 MB | 218 ms, 2.2 MB | 248 ms, 2.1 MB | 2,219 ms, 9.9 MB |
| k-nucleotide, fasta n = 1 million | **157 ms**, 25 MB | 176 ms, 23 MB | 343 ms, 25 MB | 412 ms, 39 MB | 2,568 ms, 31 MB |
| reverse-complement, fasta n = 1 million | 16.3 ms, 17 MB | **15.8 ms**, 16 MB | 17.6 ms, 16 MB | 18.5 ms, 20 MB | 75.7 ms, 30 MB |
| regex-redux, fasta n = 1 million | **97.3 ms**, 43 MB | — | 139 ms, 52 MB | 3,871 ms, 60 MB | 1,268 ms, 102 MB |
| pidigits, 10,000 digits | **569 ms**, 4.6 MB | — | 3,533 ms, 2.9 MB | 868 ms, 8.8 MB | 1,900 ms, 9.7 MB |

A program that does nothing peaks at 2.1 to 2.2 MB on Linux, as the runner measures it, so that much there is a program that holds next to nothing.

- **n-body, spectral-norm, mandelbrot and fasta** are plain loops over doubles and bytes. Calx and C, which the same clang compiles, come out within about 5% of each other, and Rust within 9% of Calx either way. Its n-body takes 7 to 9% less time. Each version unrolls the loop over the five bodies' ten pairs, and LLVM's SLP vectorizer packs pairs into two-lane vector operations: clang 23 packs all ten and pays for it in shuffles and spills, where Rust's LLVM 21 leaves two as they are (CLX-301). It's a knife-edge of the vectorizer's cost model, not of what Calx emits, as the C program comes out the same: one setting of its threshold made the C program faster than Rust's, and the settings either side of it made it up to 80% slower. Rust's fasta takes up to 9% more. Go's n-body takes twice as long as the others.
- **fannkuch-redux** takes 12% less time in Calx than in C, Rust or Go on Windows, and is even with them on Linux. It's the same loop: the C that Calx emits for the flips is the C program's, with bounds checks, so the Windows lead is how clang happened to lay it out there.
- **binary-trees** makes a node at a time on the heap, so it times each language's allocator. On Windows, C's `malloc` and Rust's default allocator both use the system's heap, and take twice as long as Calx's own allocator. On Linux glibc's `malloc` is quicker, and they take 1.3 and 1.4 times as long. Go's collector takes 7 to 10% less time than Calx. Calx holds the most memory, 66 to 68 MB to their 34 to 41, since its allocator keeps freed blocks for the next ones of their size ([std.memory](std/memory.md)).
- **reverse-complement** finds where lines end with `std.bytes`'s `find_byte`, which looks at 16 or 32 bytes at once as C's `memchr` does (CLX-299). Each line's bases are added from the input with `extend` on a slice, which copies them straight across with no list made of them first, and `reverse` swaps 16 bytes from each end at a time, where a loop of swaps from both ends wasn't vectorized (CLX-308). On Windows Calx's takes 22.8 ms, Rust's 21% longer and C's 31%. On Linux it's within 3% of C's, 16.3 ms to 15.8, and Rust's takes 8% longer.
- **k-nucleotide** counts k-mers in each language's hash map. Rust's standard map, which hashes with SipHash to resist flooding, takes 2.2 to 2.3 times as long as Calx's; many Rust programs swap in a faster hasher. Go's map takes 2.6 to 2.8 times as long, and C's, written for the program, 12 to 15% longer.
- **regex-redux** is Calx's: Rust's regex crate takes 31% longer on Windows and 43% on Linux. Each motif, such as `agggtaaa|tttaccct`, is a few strings of one length, which `std.regex` finds without a DFA, testing 32 places at a time for their first 4 bytes, as Rust's regex finds them with a SIMD search for several literals at once (CLX-297). Go's `regexp` takes 37 to 40 times as long as Calx's, since it never builds a DFA.
- **pidigits** is Calx's: Go's `math/big`, whose inner loops are assembly, takes half again as long. Each digit multiplies a big number by a small one and divides two that differ by at most a limb, and `std.bigint` does both in one pass, with no general division (CLX-298). Rust's num-bigint takes 6 to 9 times as long as Calx's.

## Realistic programs

What programs spend their time on outside the Benchmarks Game: reading and writing JSON and CSV, taking logs and text apart, sorting, hash maps, many small objects, files, compression, hashes and images.

On Windows 11:

| Benchmark | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| JSON: 200,000 records parsed, written, read typed | **216 ms**, 293 MB | — | 352 ms, 424 MB | 824 ms, 444 MB | 1,171 ms, 460 MB |
| CSV report: a million rows summed by group | **89.1 ms**, 40 MB | 236 ms, 40 MB | 165 ms, 5.2 MB | 185 ms, 12 MB | 800 ms, 12 MB |
| Log analysis: a million access-log lines | **158 ms**, 129 MB | 266 ms, 124 MB | 550 ms, 138 MB | 426 ms, 227 MB | 1,490 ms, 35 MB |
| Word count: 5 million words | **114 ms**, 95 MB | 167 ms, 50 MB | 210 ms, 89 MB | 457 ms, 472 MB | 998 ms, 380 MB |
| Sorting: 5M integers, 5M doubles, 1M strings | **245 ms**, 285 MB | 1,276 ms, 102 MB | 368 ms, 186 MB | 873 ms, 201 MB | 2,946 ms, 589 MB |
| Hash maps: 2M integer keys, 500,000 strings | **223 ms**, 280 MB | 422 ms, 202 MB | 622 ms, 239 MB | 522 ms, 170 MB | 2,153 ms, 370 MB |
| Many small objects: 4M records | **143 ms**, 52 MB | 682 ms, 54 MB | 571 ms, 56 MB | 538 ms, 89 MB | 6,244 ms, 161 MB |
| Building and splitting text: a million lines | **90.6 ms**, 78 MB | 201 ms, 88 MB | 170 ms, 89 MB | 297 ms, 171 MB | 655 ms, 209 MB |
| Files: 2,000 written, read and removed | **5,585 ms**, 4.1 MB | 6,067 ms, 4.0 MB | 6,086 ms, 4.3 MB | 6,347 ms, 12 MB | 6,366 ms, 13 MB |
| Compression: gzip, zstd and xz round trips | 1,102 ms, 101 MB | — | 1,731 ms, 92 MB | **321 ms**, 115 MB | 1,209 ms, 117 MB |
| Hashing: SHA-256 and CRC-32 of 270 MB | 107 ms, 18 MB | — | **105 ms**, 18 MB | 108 ms, 21 MB | 131 ms, 28 MB |
| PNG: a 2,048 by 2,048 image encoded and decoded | **238 ms**, 53 MB | — | 705 ms, 33 MB | 304 ms, 63 MB | — |

On Ubuntu 26.04 in WSL2:

| Benchmark | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| JSON: 200,000 records parsed, written, read typed | **240 ms**, 291 MB | — | 322 ms, 451 MB | 858 ms, 452 MB | 800 ms, 459 MB |
| CSV report: a million rows summed by group | **79.4 ms**, 38 MB | 166 ms, 38 MB | 104 ms, 2.2 MB | 162 ms, 7.3 MB | 558 ms, 10 MB |
| Log analysis: a million access-log lines | **169 ms**, 128 MB | 223 ms, 122 MB | 526 ms, 137 MB | 429 ms, 222 MB | 1,195 ms, 34 MB |
| Word count: 5 million words | **129 ms**, 93 MB | 178 ms, 48 MB | 238 ms, 87 MB | 515 ms, 466 MB | 985 ms, 364 MB |
| Sorting: 5M integers, 5M doubles, 1M strings | **269 ms**, 282 MB | 1,234 ms, 136 MB | 326 ms, 182 MB | 886 ms, 195 MB | 3,051 ms, 588 MB |
| Hash maps: 2M integer keys, 500,000 strings | **270 ms**, 278 MB | 511 ms, 204 MB | 687 ms, 240 MB | 609 ms, 164 MB | 1,990 ms, 376 MB |
| Many small objects: 4M records | **145 ms**, 50 MB | 626 ms, 57 MB | 421 ms, 57 MB | 568 ms, 74 MB | 3,733 ms, 158 MB |
| Building and splitting text: a million lines | **113 ms**, 76 MB | 126 ms, 76 MB | 130 ms, 76 MB | 319 ms, 178 MB | 601 ms, 205 MB |
| Files: 2,000 written, read and removed | 19.9 ms, 2.4 MB | 20.2 ms, 2.1 MB | **19.7 ms**, 2.2 MB | 31.4 ms, 6.8 MB | 64.6 ms, 9.6 MB |
| Compression: gzip, zstd and xz round trips | 1,146 ms, 100 MB | — | 1,571 ms, 94 MB | **353 ms**, 111 MB | 1,378 ms, 128 MB |
| Hashing: SHA-256 and CRC-32 of 270 MB | 106 ms, 16 MB | — | **104 ms**, 16 MB | 109 ms, 16 MB | 148 ms, 29 MB |
| PNG: a 2,048 by 2,048 image encoded and decoded | **241 ms**, 51 MB | — | 705 ms, 31 MB | 305 ms, 58 MB | — |

- **JSON** parses 200,000 records into a tree, writes the tree back as text, and reads that into typed records. Calx's `std.json` takes 61% of serde_json's time on Windows and 75% on Linux, and holds 31 to 35% less memory. Its objects are `LinkedMap<String, Json>`, whose keys of 15 bytes or fewer are kept in the table's slots (CLX-316). Go's `encoding/json` works by reflection and takes 3.6 to 3.8 times as long as Calx, longer than Python's on Linux.
- **CSV** reads a million rows with each language's CSV reader, some fields quoted, and sums them by group. Calx's `std.csv` takes about half the time of Rust's csv crate and Go's `encoding/csv` on Windows, each reading into one reused row, and on Linux 76% of Rust's and half of Go's. But it reads from text in memory, so it holds the whole 40 MB file, where they stream it in 2 to 12 MB. C reads its fields by hand, a byte at a time; on Windows about a third of its time goes to the C runtime's `snprintf` and `strtod`.
- **Log analysis** is Calx's. The Calx version is written as a script would be, with `split_once` and `split`, and the parts of a line it binds with `let` and only looks at are views into the line, not copies (CLX-293). Its one-byte searches look over the first 64 bytes inline, a `split` it only indexes by number stops at the highest index, and checking that the file is UTF-8 goes 64 bytes at a time (CLX-309). The path it trims, which a `var` holds, is a view of the line too (CLX-310), and the two lists it splits each line into are on the stack, so splitting a line allocates nothing (CLX-314). Calx's set of addresses, timed alone, took about two-thirds of the time C's does (CLX-313), and now the addresses, 10 to 12 bytes, the methods and most paths are kept in their tables' slots, with no buffer of their own: an address put in with `ip.copy()` goes in as a view of the line and is copied into its slot (CLX-316). Timed in turns against the build before, 20 times each, that took 17% off Calx's time on Windows and 25% on Linux. C scans each line in place with `memchr` and copies nothing, and takes 68% longer than Calx on Windows and 32% longer on Linux, though Calx also checks that the file is UTF-8, which C doesn't. C's times are rough: its runs swing by a fifth or more from one to the next, and on Linux log analysis was recorded a second time, as in the first C's runs swung by half. Go's `strings.Cut` gives parts that share the line too, and takes 2.5 to 2.7 times as long as Calx. Rust's `&str` parts copy nothing either, and with std's maps, which hash with SipHash, it takes 3.1 to 3.5 times as long.
- **Word count** is Calx's: C takes 38 to 46% longer, though its hand-written map keeps its keys pointing into the text and holds half the memory. Each word the Calx program looks up is a view into the text, and only a word the map hasn't seen is copied, to be its key, into the map's slot itself when it's 15 bytes or fewer (CLX-316).
- **Sorting** is Calx's: Rust's `sort_unstable` takes 21 to 50% longer. `sort_by` with `total_cmp` on the 5 million doubles is a radix sort of their bits (CLX-292), 60 to 66 ms measured alone on Windows to Rust's 92, and the integers and strings take 44 ms to Rust's 61 and 46 to its 133. C's `qsort` calls a comparison function for every comparison and takes 4.6 to 5.2 times as long as Calx. The radix sort's second list is why Calx holds the most memory here.
- **Hash maps** use each language's map with its default hash. Rust's, which hashes with SipHash, takes 2.5 to 2.8 times as long as Calx's, Go's 2.3 times, and C's open addressing, written for the program, 1.9 times. The string keys are 8 to 16 bytes, and Calx keeps all but the 16-byte ones in the table's slots (CLX-316). Calx holds more memory than C, Rust or Go here, as its allocator keeps big freed blocks, such as a table's arrays from before it grew, to use again (CLX-318).
- **Many small objects** makes 4 million records of three allocations each, keeps one in 16 and frees the rest, so like binary-trees it times the allocator. C and Rust take 4 to 5 times as long as Calx on the Windows heap, and 2.9 to 4.3 times with glibc's on Linux, and Go's collector 3.8 to 3.9 times.
- **Text** builds a million lines with formatting and takes them apart again. On Windows Rust takes 88% more time than Calx, and C, growing its buffers by doubling and formatting with `snprintf`, 2.2 times as much. On Linux it's closer: C takes 12% longer and Rust 15%. C's and Rust's take 24 to 37% less time there than on Windows, with glibc's `snprintf` and `malloc`, and Calx's 25% more. Calx's own work is no slower on Linux; under WSL, touching fresh memory costs this program about 28 ms in the first round, in every language alike (CLX-300). Calx's `split` sets up its search once for all the parts, the fields a line is split into are views, and their list is on the stack (CLX-314).
- **Files** writes 2,000 small files, reads them back and removes them. On Windows that's the file system's time and Windows Defender's, which looks at each new file, so every language, Python too, takes 5.6 to 6.4 seconds, and the runs vary by a tenth. On Linux the same takes 20 ms in Calx, C and Rust, 31 in Go and 65 in Python.
- **Compression** is Go's, for less work: its gzip writes a file 8 to 15% bigger than the others', and its xz, from ulikunitz/xz, one 38% bigger than liblzma's. Rust's and Python's xz is liblzma, 1.37 s of Rust's 1.73 s. Calx's `std.xz` writes within 1% of liblzma's size in less time, and its zstd and xz would use every processor; here the Calx program sets one worker, as the others compress on one thread.
- **Hashing** is even: SHA-256 and CRC-32 run on the processor's own instructions in Calx, Rust and Go alike.
- **PNG** writes zlib at its default level with each row's filter chosen as libpng chooses it, in every language. The png crate's encoder starts at a fast setting that wrote a file nearly twice the size, so the Rust program sets the same work as the others. Rust then takes about 3 times as long as Calx, and Go 27% longer.

The compressed sizes, in bytes, of the 17 MB log, and of its first 4 MiB for xz:

| | Calx | Rust | Go | Python |
| --- | --- | --- | --- | --- |
| gzip, level 6 | 2,024,744 | 1,926,701 | 2,219,112 | 2,054,481 |
| zstd, level 3 | 2,479,533 | 2,493,814 | 2,504,046 | 2,471,087 |
| xz, preset 6 | 442,284 | 437,936 | 602,856 | 437,936 |
| PNG, the 12 MB image | 4,326,572 | 4,372,602 | 4,495,405 | — |

## Parallel loops

Calx's [`parallel for`](reference/parallel.md) against Rust's Rayon, Go's goroutines and C's OpenMP, at 1, 4 and 8 threads and on all 32 processors. Each program takes its thread count as an argument: `std.pool.set_workers` in Calx, Rayon's `ThreadPoolBuilder`, `GOMAXPROCS` with one goroutine a thread, and `omp_set_num_threads`. Up to 7 threads run on the performance cores, one on each, 8 on all 8, and 32 on every processor, efficiency cores too.

- **mandelbrot**, 6,000 by 6,000, a row an iteration. Rows near the set's middle take far longer than those at the edge.
- **spectral-norm**, n = 5,500: 40 matrix-vector products, each split by rows. Every element is summed in order by one thread, so every language gets the same digits at any count.
- **π as a float sum**: the midpoint rule over 2 billion strips, the classic OpenMP reduction example. A float sum's last bits depend on the order its terms are added in. A `parallel for` can't add to one total, so the Calx program gives each of 1,024 chunks a sum of its own and adds those in order after the loop. Rust uses Rayon's `sum`, C an OpenMP `reduction`, and Go a sum for each goroutine's equal run of strips, each its language's usual way.

The median wall time on Windows 11, the fastest in bold, and whether each language printed the same, its last bits too, at every count and in every run:

mandelbrot, 6,000 by 6,000:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | 1,412 ms | 1,403 ms | 1,411 ms | **1,357 ms** |
| 4 | 364 ms | 361 ms | 361 ms | **346 ms** |
| 8 | 192 ms | 191 ms | 189 ms | **183 ms** |
| 32 | 80.8 ms | 83.5 ms | 78.1 ms | **76.8 ms** |
| Same output at every count | yes | yes | yes | yes |

spectral-norm, n = 5,500:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | 911 ms | 911 ms | **900 ms** | 905 ms |
| 4 | 235 ms | 234 ms | **232 ms** | 236 ms |
| 8 | 129 ms | 140 ms | **126 ms** | 142 ms |
| 32 | 80.0 ms | 88.0 ms | **79.5 ms** | 96.6 ms |
| Same output at every count | yes | yes | yes | yes |

π as a float sum, 2 billion terms:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | **1,502 ms** | 1,506 ms | 1,672 ms | 1,509 ms |
| 4 | **378 ms** | 380 ms | 421 ms | 384 ms |
| 8 | **199 ms** | 211 ms | 219 ms | 210 ms |
| 32 | **118 ms** | 129 ms | 126 ms | 137 ms |
| Same output at every count | yes | no | no | no |

On Ubuntu 26.04 in WSL2, mandelbrot, 6,000 by 6,000:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | 1,409 ms | 1,402 ms | 1,466 ms | **1,336 ms** |
| 4 | 359 ms | 358 ms | 372 ms | **339 ms** |
| 8 | 191 ms | 194 ms | 199 ms | **178 ms** |
| 32 | 76.9 ms | 83.2 ms | 80.6 ms | **71.5 ms** |
| Same output at every count | yes | yes | yes | yes |

spectral-norm, n = 5,500:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | 910 ms | 915 ms | 904 ms | **902 ms** |
| 4 | **229 ms** | 244 ms | 234 ms | 234 ms |
| 8 | **128 ms** | 177 ms | 137 ms | 168 ms |
| 32 | **82.4 ms** | 151 ms | 128 ms | 99.9 ms |
| Same output at every count | yes | yes | yes | yes |

π as a float sum, 2 billion terms:

| Threads | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| 1 | 1,489 ms | 1,511 ms | 1,642 ms | **1,480 ms** |
| 4 | 375 ms | 383 ms | 415 ms | **373 ms** |
| 8 | 215 ms | 211 ms | 229 ms | **208 ms** |
| 32 | 144 ms | 146 ms | **129 ms** | 136 ms |
| Same output at every count | yes | no | no | no |

- **Only Calx's float sum comes out the same at every count**, on both systems. Its 1,024 chunks are the program's, so the sum is added in the same order whatever runs it: `400921fb54442d22` with 1 thread or 32. Go's partial sums follow its thread count, so each count gives other last bits. C's OpenMP reduction and Rust's Rayon `sum` gave other bits from one run to the next at the same count, since which thread's partial sum is added first depends on timing: OpenMP at 4 threads, Rayon at 4, 8 and 32. Every language printed the same π to six places. A Rust or C program can get Calx's result by summing fixed chunks too; Calx's checker makes that the way to write it, since an iteration can't add to a total another one adds to.
- **Uneven rows.** Mandelbrot's rows differ in cost. C's dynamic schedule and Go's shared counter hand out a row at a time, and Calx's pool splits the loop into chunks, up to 64 a thread, each at least 20 µs of work once it knows what an iteration takes, and threads that run out take half of another's chunks (CLX-296). Calx is within 2% of C and Rust at 4 and 8 threads, and on all 32 within 3.5% of Rust on Windows and ahead of both on Linux. Go takes 5 to 8% less time than Calx at 4 threads and up, about what it takes less at 1, 4 to 5.5%, so what's left is the loop itself, not how it's shared out.
- **Even work is closer.** In spectral-norm Calx is within 3% of Rayon, the fastest, at every count on Windows, and the fastest at 4 threads and up on Linux. At the sum it's the fastest at every count on Windows, and on Linux within 4% of Go's up to 8 threads.
- **On every processor** the efficiency cores join in, and the two systems differ. On Windows, Rayon and Calx are even at spectral-norm, and OpenMP's default schedule and the Go program, which split the rows evenly up front, take 10 and 21% longer than Calx. On Linux, Calx is fastest by far: Go takes 21% longer, and Rayon and OpenMP 55 and 83%. We haven't traced why. Our guess is that the two systems place threads on the efficiency cores differently, and an even share that lands on one finishes last. Under WSL these times also swing from one recording to the next: Calx's float sum on all 32 took 121, 144 and 118 ms in three runs on the same day, and Rust's 127, 129 and 152, so the Linux rows for 32 threads are rough.
- **Python sits these out.** Its threads take turns under its global lock, so a loop over threads runs on one core; a program would use processes instead, which is other work.

## Building and starting

How long it takes to build a program, how big the program is, and how long it takes to start. Two programs in each compiled language: hello, and a generated one of 64 modules of about 150 lines each, each with a struct, an enum and a match, a list of strings and eight small hashing loops, every module calling the one before it, written in each language as it would be ([`bench/dx.calx`](../bench/dx.calx)). They print the same in every language.

- **From scratch:** with every cache empty. Calx compiles its runtime too, as it does once per build mode, and Go compiles its runtime and standard library into an empty `GOCACHE`. Rust and C link a standard library that came compiled.
- **One module edited:** a constant in module 0 changed, with the caches from the build before, which is the build a developer waits for most.
- **Nothing changed:** the same build again.
- **How each builds:** Calx with `calx build`, C with ninja, one object a file and then a link, Rust with Cargo, one crate with a module a file, and Go with `go build`, one package with a file a module. Debug builds are Calx's default, `-O0 -g` for C, Cargo's dev profile, and `go build -gcflags=all=-N -l` for Go, which turns off its optimizations as debuggers want. Release builds are the flags above. Builds use every processor, as they would on a developer's machine.

The median wall time of each build on Windows 11, 5 rounds, the fastest in bold. The 64-module program is 9,922 lines in Calx, 9,985 in Rust, 10,502 in Go, and 11,014 in C with its headers.

| Build | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| hello, from scratch, debug | 315 ms | **138 ms** | 257 ms | 1,808 ms |
| hello, from scratch, release | 436 ms | **98.6 ms** | 813 ms | 2,175 ms |
| 64 modules, from scratch, debug | 546 ms | **509 ms** | 863 ms | 1,776 ms |
| 64 modules, from scratch, release | 771 ms | **575 ms** | 1,726 ms | 2,126 ms |
| 64 modules, one module edited, debug | **120 ms** | 175 ms | 525 ms | 504 ms |
| 64 modules, one module edited, release | **211 ms** | 230 ms | 1,685 ms | 465 ms |
| 64 modules, nothing changed, debug | **7.70 ms** | 8.50 ms | 69.5 ms | 101 ms |
| 64 modules, nothing changed, release | 8.80 ms | **8.40 ms** | 68.9 ms | 104 ms |

The release programs' sizes:

| Program | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| hello | 181 KB | 111 KB | 126 KB | 2,429 KB |
| 64 modules | 423 KB | 344 KB | 154 KB | 2,701 KB |

Starting hello and running it to the end, timed from outside, pinned as the single-threaded benchmarks are:

| Program | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| hello | 3.56 ms, 3.8 MB | **3.47 ms**, 3.7 MB | 3.98 ms, 4.1 MB | 4.96 ms, 5.4 MB | 17.2 ms, 11 MB |

On Ubuntu 26.04 in WSL2, 5 rounds, with the compiler, its runtime and its library copied into Linux's file system, as an installed Calx is:

| Build | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| hello, from scratch, debug | 188 ms | **101 ms** | 220 ms | 1,321 ms |
| hello, from scratch, release | 590 ms | **181 ms** | 1,686 ms | 1,710 ms |
| 64 modules, from scratch, debug | 279 ms | **199 ms** | 624 ms | 1,319 ms |
| 64 modules, from scratch, release | 843 ms | **445 ms** | 2,415 ms | 1,723 ms |
| 64 modules, one module edited, debug | **62.9 ms** | 125 ms | 293 ms | 257 ms |
| 64 modules, one module edited, release | **116 ms** | 330 ms | 2,346 ms | 365 ms |
| 64 modules, nothing changed, debug | **1.60 ms** | **1.60 ms** | 10.5 ms | 151 ms |
| 64 modules, nothing changed, release | **1.60 ms** | 1.70 ms | 10.8 ms | 144 ms |

| Program | Calx | C | Rust | Go |
| --- | --- | --- | --- | --- |
| hello | 95 KB | 6 KB | 415 KB | 2,285 KB |
| 64 modules | 577 KB | 217 KB | 447 KB | 2,548 KB |

| Program | Calx | C | Rust | Go | Python |
| --- | --- | --- | --- | --- | --- |
| hello | 0.37 ms, 2.2 MB | **0.29 ms**, 2.2 MB | 0.40 ms, 2.2 MB | 0.86 ms, 2.1 MB | 6.87 ms, 9.1 MB |

- **After an edit, Calx builds first** on both systems. It rebuilt the 64-module program after a one-module edit in 63 to 120 ms in debug and 116 to 211 ms in release, against 125 to 175 and 230 to 330 for C with ninja, 257 to 504 and 365 to 465 for Go, and 293 to 525 and 1,685 to 2,346 for Cargo: a Rust release build with thin LTO and one codegen unit compiles the whole crate again.
- **From scratch, C is first.** Calx compiles its runtime in each mode the first time, which is most of hello's 188 to 590 ms, and Go compiles its runtime and standard library into an empty cache, 1.3 to 2.2 seconds. Rust and C link a standard library that came compiled.
- **With nothing changed**, Calx sees it by the files' sizes and times, in 7.7 to 8.8 ms on Windows and 1.6 ms on Linux, as long as ninja takes on each. Cargo takes 69 and 11 ms, and Go 101 to 151 ms. Run from the Windows drive, which WSL reads through a file share, the same no-op build takes Calx 15 to 19 ms, as starting the compiler from there takes about 6 ms and each of its own files a build checks about 1 ms more, so the Linux builds here use a copy (CLX-302).
- **Sizes.** Go programs carry its runtime and their symbol tables, 2.2 to 2.6 MB. Calx programs carry their runtime too: hello is 181 KB on Windows, 70 KB more than C's there, and 95 KB on Linux, where C's is 6 KB since it uses the system's C library as it is. Rust's 64-module program comes out smallest on Windows, 154 KB, as LTO inlines most of it; on Linux it's 447 KB.
- **Starting** a compiled hello takes 3.5 to 5 ms on Windows, most of it the system making a process, since C's, which does nothing else, takes 3.5. On Linux it takes 0.3 to 0.9 ms. Calx's takes 3% longer than C's on Windows and 0.08 ms longer on Linux. Python takes 17 and 7 ms, as it starts its interpreter.

## What isn't measured here

- **Other languages.** Java, C# and Zig aren't here. Java 25 is installed on the machine these ran on, but no Java versions are written yet; there's no .NET SDK or Zig there.
- **C's libraries.** C sits out six benchmarks, since what they need isn't in its standard library, and its hash maps are the ones written for each program. A C program built on a tuned library would be faster than ours.
- **One machine.** Every number is from one i9-13900K, under Windows 11 and under Ubuntu in WSL2 on the same machine. Processors with other caches, and Linux on its own hardware, will differ.
- **CPU time on Windows** counts in ticks of the system's clock, every 15.6 ms, so the record's CPU times there are rough for short programs. The tables give wall time.

## Running it

```sh
calx run --release bench/suite.calx                       # everything, 5 rounds
calx run --release bench/suite.calx -- --groups compute    # one group
calx run --release bench/suite.calx -- --only json,csv --langs calx,rust
calx run --release bench/suite.calx -- --quick             # small inputs: does everything build and agree?
calx run --release bench/suite.calx -- --record            # written to bench/results/suite-<os>.json
calx run --release bench/suite.calx -- --tables            # this page's tables and tallies, from the records
```

It needs clang, Cargo, Go, Python 3 and ninja on the `PATH`. [`bench/README.md`](../bench/README.md#against-c-rust-go-and-python) says more about each step.
