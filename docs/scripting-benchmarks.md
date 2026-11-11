---
title: Scripting benchmarks
description: Calx timed against Rust and Python on what scripts do, taking text apart, writing numbers, JSON, walking a tree of files and starting up, with the CPU time and memory each used.
---

# Scripting benchmarks

What a script does, timed in Calx, Rust and Python: taking text apart, writing numbers as text, reading and writing JSON, walking a tree of files, and starting up. Each is a program in each language that does the same work with the same algorithms, written as that language usually writes it, with its standard library or the crates Rust code usually reaches for. Each prints a total its work came to, which must be the same in all three, or the run doesn't count. They're in [`bench/`](../bench/README.md#calx-as-a-scripting-language), with the runner, [`bench/script.calx`](../bench/script.calx), and the recorded results in [`bench/results`](../bench/results).

These are our benchmarks, of a few kinds of work, so they say how Calx does on work shaped like this. Python's string methods, its `json` module and its file functions are written in C, so its times are mostly C's. If you can make one of the Rust or Python programs faster without changing what it computes, please send a pull request: the numbers will change with it.

## How it's measured

- Each program is run 5 times as a fresh process, the languages taking turns, in the other order every other round. A program times each of its steps, the best of 7, and the tables give each step's best over the 5 runs. The fastest of the three is in bold.
- **CPU time** is what all the process's threads used, in the kernel and out, from its start to its end, generating its input included, and **memory** the most it held in RAM at once: its peak working set on Windows and its peak resident set on Linux. Both are the median of the 5 runs. Windows counts CPU time in ticks of its clock, every 15.6 ms, so a program that ends sooner shows 0 or 15.6 ms there.
- On an Intel Core i9-13900K, with other programs open but otherwise quiet. Under Windows 11 everything ran on the performance cores, out of power throttling; under Ubuntu 26.04 in WSL2 on the same machine, on one of them, with `taskset`.
- Calx at commit 6e0437a, built with clang 23. Rust 1.91.1 on Windows and 1.91.0 on Linux, with `-C opt-level=3`, one codegen unit, `panic = "abort"` and `target-cpu=x86-64-v3`, as Calx builds, and thin LTO for the two with crates. Python 3.14.6 on Windows and 3.14.4 on Linux.

## Taking text apart

[`bench/text`](../bench/text): 200,000 lines of a CSV, 10.4 MB. The fields of every line, trimmed, in loops over `lines()` and `split(",")`; the lines joined with `|`; and that text with a word replaced. Rust uses `str`'s `lines`, `split`, `trim`, `join` and `replace`, and Python `str`'s.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.25 ms** | 8.65 ms | 95.3 ms |
| The lines joined | **1.25 ms** | 5.35 ms | 14.0 ms |
| A word replaced | **2.15 ms** | 3.36 ms | 5.25 ms |
| CPU time, the whole run | **62.5 ms** | 141 ms | 906 ms |
| Most memory held | 43.3 MB | **33.7 MB** | 72.3 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.95 ms** | 8.35 ms | 69.4 ms |
| The lines joined | **1.43 ms** | 5.87 ms | 13.3 ms |
| A word replaced | **2.36 ms** | 4.55 ms | 3.68 ms |
| CPU time, the whole run | **78.9 ms** | 162 ms | 709 ms |
| Most memory held | 41.4 MB | **35.1 MB** | 69.3 MB |

## Writing numbers as text

[`bench/format`](../bench/format): each onto a new string, 200,000 doubles from 1 to 1,000 to three places, `{v:.3}`; 200,000 integers below 1,048,576 padded to 8, `{i:>8}`; a million of them, `{i}`; and the 200,000 doubles in their shortest digits, `{v}`. Rust writes with `write!` into a `String`, and Python joins f-strings with `"".join`, the quickest way it has to put many pieces together. All three write the same digits.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **7.53 ms** | 16.5 ms | 36.2 ms |
| 200,000 integers, `{i:>8}` | **1.47 ms** | 3.58 ms | 29.3 ms |
| A million integers, `{i}` | **5.50 ms** | 13.0 ms | 84.7 ms |
| 200,000 doubles, `{v}` | **5.01 ms** | 11.9 ms | 65.7 ms |
| CPU time, the whole run | **156 ms** | 328 ms | 1,781 ms |
| Most memory held | 41.7 MB | **28.6 MB** | 134.7 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **8.36 ms** | 16.3 ms | 28.8 ms |
| 200,000 integers, `{i:>8}` | **1.56 ms** | 3.95 ms | 26.3 ms |
| A million integers, `{i}` | **5.72 ms** | 13.6 ms | 61.7 ms |
| 200,000 doubles, `{v}` | **5.44 ms** | 11.7 ms | 54.6 ms |
| CPU time, the whole run | **165 ms** | 337 ms | 1,435 ms |
| Most memory held | 39.6 MB | **24.7 MB** | 142.1 MB |

[Printing](reference/printing.md#formats) says how Calx writes them.

## Reading and writing JSON

[`bench/json`](../bench/json): 100,000 player records, 12.6 MB, each with an id, a name, a position, a list of tags, a flag and a level. The text parsed into a tree, the tree written back as compact text, and the text read into typed records. Calx uses [`std.json`](std/json.md) and `@json` structs, Rust serde_json's `Value` and `#[derive(Deserialize)]`, and Python `json` and dataclasses made from its dicts.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **31.2 ms** | 74.8 ms | 117 ms |
| Written back | **20.3 ms** | 24.8 ms | 105 ms |
| Read into typed records | **14.5 ms** | 26.6 ms | 221 ms |
| CPU time, the whole run | **594 ms** | 1,250 ms | 4,312 ms |
| Most memory held | **201.0 MB** | 215.4 MB | 281.7 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **31.3 ms** | 35.0 ms | 80.4 ms |
| Written back | 21.1 ms | **19.8 ms** | 88.7 ms |
| Read into typed records | **15.2 ms** | 18.1 ms | 103 ms |
| CPU time, the whole run | **619 ms** | 781 ms | 2,484 ms |
| Most memory held | **199.2 MB** | 231.5 MB | 289.4 MB |

- Both read typed records straight from the text, with no tree between: serde's derive, and `@json`'s `from_text`, which takes 55% of serde's time on Windows and 84% on Linux. With no tree, the Calx program also holds less memory than the Rust one at its most.
- serde_json's parse takes twice as long on Windows as on Linux, where Calx's takes the same. Rust programs use each system's allocator. On Linux serde_json's parse takes 12% longer than Calx's, and its writing 6% less.

## A tree of files

[`bench/walk`](../bench/walk): 10,000 small files in 420 directories, a third each `.calx`, `.txt` and `.json`, in the temp folder. Everything in the tree walked, each file and directory counted, and the paths `**/*.calx` matches found, with [`std.fs`](std/fs.md)'s `walk` and `glob`, Rust's walkdir and glob crates, and Python's `os.walk` and `glob.glob`. Each sorts the names, as `std.fs` does.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **15.5 ms** | 16.3 ms | 29.9 ms |
| `**/*.calx` found | **15.4 ms** | 17.4 ms | 41.7 ms |
| CPU time, the whole run | **234 ms** | 250 ms | 562 ms |
| Most memory held | 6.1 MB | **4.8 MB** | 13.3 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **1.65 ms** | 2.68 ms | 5.17 ms |
| `**/*.calx` found | **1.90 ms** | 2.55 ms | 7.20 ms |
| CPU time, the whole run | **29.7 ms** | 42.1 ms | 107 ms |
| Most memory held | 3.6 MB | **2.9 MB** | 11.4 MB |

All three take 6 to 9 times as long on Windows as on Linux.

## Starting up

[`bench/start`](../bench/start): the smallest program, which prints a line and ends, timed from outside, the best of 20 runs in each of 5 rounds. Then as a script, the best of 25: changed, built and run, which for Calx is `calx run`, for Rust `rustc`, which builds for debugging as `cargo run` does, and then the program, and for Python `python`; and run again unchanged.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **3.75 ms** | 4.00 ms | 18.1 ms |
| Its CPU time | 0 ms | 0 ms | 15.6 ms |
| Most memory held | **3.9 MB** | 4.2 MB | 10.7 MB |
| Changed, then run | 108 ms | 161 ms | **18.1 ms** |
| Run again, unchanged | 26.4 ms | **4.42 ms** | 18.1 ms |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **0.30 ms** | 0.31 ms | 6.25 ms |
| Its CPU time | **0.27 ms** | 0.28 ms | 6.53 ms |
| Most memory held | 2.9 MB | 2.9 MB | 9.0 MB |
| Changed, then run | 52.0 ms | 153 ms | **6.25 ms** |
| Run again, unchanged | 16.1 ms | **0.35 ms** | 6.25 ms |

- Rust's last row is its program alone, built before; Python reads and compiles the script each time it runs it, so its rows are the same time.
- `calx run` of a script it has built checks that nothing the script was built from has changed, and then runs what it built: 16 to 23 ms more than the program alone.

## What they show

On these benchmarks, Calx takes a quarter to two thirds of Rust's time to take text apart and write numbers, 5 to 38% less to walk a tree of files, and a little less to start. It parses JSON into a tree faster, writes it about as fast, and reads typed records in 55 to 84% of serde's time. Python takes 1.6 to 29 times as long as Calx at each step, least where its C does most of the work, as in replacing a word, but it runs a small script, changed or not, sooner than `calx run` does.

Calx held 18 to 60% more memory than Rust to take text apart, write numbers and walk a tree of files, and 7 to 14% less to read and write JSON. The smallest program held the same.
