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
- Calx at commit 2376087, built with clang 23. Rust 1.91.1 on Windows and 1.91.0 on Linux, with `-C opt-level=3`, one codegen unit, `panic = "abort"` and `target-cpu=x86-64-v3`, as Calx builds, and thin LTO for the two with crates. Python 3.14.6 on Windows and 3.14.4 on Linux.

## Taking text apart

[`bench/text`](../bench/text): 200,000 lines of a CSV, 10.4 MB. The fields of every line, trimmed, in loops over `lines()` and `split(",")`; the lines joined with `|`; and that text with a word replaced. Rust uses `str`'s `lines`, `split`, `trim`, `join` and `replace`, and Python `str`'s.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.06 ms** | 8.52 ms | 92.7 ms |
| The lines joined | **1.21 ms** | 5.17 ms | 14.0 ms |
| A word replaced | **2.23 ms** | 3.38 ms | 5.12 ms |
| CPU time, the whole run | **62.5 ms** | 141 ms | 859 ms |
| Most memory held | **34.4 MB** | 34.8 MB | 73.8 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.81 ms** | 8.24 ms | 65.1 ms |
| The lines joined | **1.20 ms** | 5.31 ms | 11.6 ms |
| A word replaced | **2.17 ms** | 4.23 ms | 3.25 ms |
| CPU time, the whole run | **66.8 ms** | 143 ms | 644 ms |
| Most memory held | **32.1 MB** | 35.1 MB | 69.4 MB |

## Writing numbers as text

[`bench/format`](../bench/format): each onto a new string, 200,000 doubles from 1 to 1,000 to three places, `{v:.3}`; 200,000 integers below 1,048,576 padded to 8, `{i:>8}`; a million of them, `{i}`; and the 200,000 doubles in their shortest digits, `{v}`. Rust writes with `write!` into a `String`, and Python joins f-strings with `"".join`, the quickest way it has to put many pieces together. All three write the same digits.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **7.33 ms** | 16.6 ms | 36.0 ms |
| 200,000 integers, `{i:>8}` | **1.38 ms** | 3.60 ms | 29.1 ms |
| A million integers, `{i}` | **5.23 ms** | 13.1 ms | 83.9 ms |
| 200,000 doubles, `{v}` | **4.74 ms** | 11.9 ms | 65.1 ms |
| CPU time, the whole run | **141 ms** | 344 ms | 1,797 ms |
| Most memory held | **28.3 MB** | 29.3 MB | 135.7 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **8.18 ms** | 16.2 ms | 28.3 ms |
| 200,000 integers, `{i:>8}` | **1.47 ms** | 3.91 ms | 25.8 ms |
| A million integers, `{i}` | **5.47 ms** | 13.5 ms | 60.8 ms |
| 200,000 doubles, `{v}` | **5.04 ms** | 11.7 ms | 54.1 ms |
| CPU time, the whole run | **153 ms** | 333 ms | 1,408 ms |
| Most memory held | 25.6 MB | **24.7 MB** | 141.9 MB |

[Printing](reference/printing.md#formats) says how Calx writes them.

## Reading and writing JSON

[`bench/json`](../bench/json): 100,000 player records, 12.6 MB, each with an id, a name, a position, a list of tags, a flag and a level. The text parsed into a tree, the tree written back as compact text, and the text read into typed records. Calx uses [`std.json`](std/json.md) and `@json` structs, Rust serde_json's `Value` and `#[derive(Deserialize)]`, and Python `json` and dataclasses made from its dicts.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **30.9 ms** | 74.2 ms | 112 ms |
| Written back | **19.5 ms** | 24.0 ms | 103 ms |
| Read into typed records | **13.4 ms** | 26.1 ms | 219 ms |
| CPU time, the whole run | **547 ms** | 1,203 ms | 4,156 ms |
| Most memory held | **167.8 MB** | 216.2 MB | 282.4 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **30.9 ms** | 34.9 ms | 79.3 ms |
| Written back | 20.5 ms | **19.6 ms** | 88.1 ms |
| Read into typed records | **13.8 ms** | 18.2 ms | 101 ms |
| CPU time, the whole run | **583 ms** | 774 ms | 2,455 ms |
| Most memory held | **165.4 MB** | 231.5 MB | 289.7 MB |

- Both read typed records straight from the text, with no tree between: serde's derive, and `@json`'s `from_text`, which takes 51% of serde's time on Windows and 76% on Linux.
- serde_json's parse takes twice as long on Windows as on Linux, where Calx's takes the same. Rust programs use each system's allocator. On Linux serde_json's parse takes 13% longer than Calx's, and its writing 4% less.

## A tree of files

[`bench/walk`](../bench/walk): 10,000 small files in 420 directories, a third each `.calx`, `.txt` and `.json`, in the temp folder. Everything in the tree walked, each file and directory counted, and the paths `**/*.calx` matches found, with [`std.fs`](std/fs.md)'s `walk` and `glob`, Rust's walkdir and glob crates, and Python's `os.walk` and `glob.glob`. Each sorts the names, as `std.fs` does.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **15.2 ms** | 15.9 ms | 30.1 ms |
| `**/*.calx` found | **15.4 ms** | 17.1 ms | 42.2 ms |
| CPU time, the whole run | 234 ms | 234 ms | 531 ms |
| Most memory held | 6.2 MB | **5.5 MB** | 13.7 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **1.66 ms** | 2.77 ms | 5.58 ms |
| `**/*.calx` found | **1.92 ms** | 2.59 ms | 7.60 ms |
| CPU time, the whole run | **36.8 ms** | 55.2 ms | 114 ms |
| Most memory held | 3.4 MB | **2.9 MB** | 11.4 MB |

All three take 5 to 9 times as long on Windows as on Linux.

## Starting up

[`bench/start`](../bench/start): the smallest program, which prints a line and ends, timed from outside, the best of 20 runs in each of 5 rounds. Then as a script, the best of 25: changed, built and run, which for Calx is `calx run`, for Rust `rustc`, which builds for debugging as `cargo run` does, and then the program, and for Python `python`; and run again unchanged.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **7.27 ms** | 7.74 ms | 22.5 ms |
| Its CPU time | 0 ms | 0 ms | 15.6 ms |
| Most memory held | **4.4 MB** | 4.9 MB | 11.5 MB |
| Changed, then run | 120 ms | 179 ms | **22.5 ms** |
| Run again, unchanged | 33.1 ms | **7.95 ms** | 22.5 ms |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **0.31 ms** | 0.32 ms | 6.27 ms |
| Its CPU time | **0.28 ms** | 0.29 ms | 6.56 ms |
| Most memory held | 2.9 MB | 2.9 MB | 9.1 MB |
| Changed, then run | 50.3 ms | 140 ms | **6.27 ms** |
| Run again, unchanged | 14.8 ms | **0.33 ms** | 6.27 ms |

- Rust's last row is its program alone, built before; Python reads and compiles the script each time it runs it, so its rows are the same time.
- `calx run` of a script it has built checks that nothing the script was built from has changed, and then runs what it built: 15 to 26 ms more than the program alone.
- Starting a process on Windows took about twice as long for all three in this recording as in the one before, 3.75 ms for Calx's program then. It did again with the machine quiet, so the time is the system's.

## What they show

On these benchmarks, Calx takes a quarter to two thirds of Rust's time to take text apart and write numbers, 4 to 40% less to walk a tree of files, and a little less to start. It parses JSON into a tree faster, writes it about as fast, and reads typed records in 51 to 76% of serde's time. Python takes 1.5 to 30 times as long as Calx at each step, least where its C does most of the work, as in replacing a word, but it runs a small script, changed or not, sooner than `calx run` does.

Calx held about as much memory as Rust to take text apart and write numbers, from 9% less to 4% more, 22 to 29% less to read and write JSON, and 13 to 17% more to walk a tree of files. The smallest program held 10% less on Windows and the same on Linux.
