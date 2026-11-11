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
- Calx at commit c98398f, built with clang 23. Rust 1.91.1 on Windows and 1.91.0 on Linux, with `-C opt-level=3`, one codegen unit, `panic = "abort"` and `target-cpu=x86-64-v3`, as Calx builds, and thin LTO for the two with crates. Python 3.14.6 on Windows and 3.14.4 on Linux.

## Taking text apart

[`bench/text`](../bench/text): 200,000 lines of a CSV, 10.4 MB. The fields of every line, trimmed, in loops over `lines()` and `split(",")`; the lines joined with `|`; and that text with a word replaced. Rust uses `str`'s `lines`, `split`, `trim`, `join` and `replace`, and Python `str`'s.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.29 ms** | 8.60 ms | 93.5 ms |
| The lines joined | **1.27 ms** | 5.40 ms | 14.7 ms |
| A word replaced | **2.16 ms** | 3.42 ms | 5.46 ms |
| CPU time, the whole run | **62.5 ms** | 125 ms | 797 ms |
| Most memory held | 43.3 MB | **34.1 MB** | 71.8 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The fields, trimmed | **3.87 ms** | 8.24 ms | 66.3 ms |
| The lines joined | **1.31 ms** | 5.64 ms | 12.1 ms |
| A word replaced | **2.31 ms** | 4.32 ms | 3.28 ms |
| CPU time, the whole run | **75.5 ms** | 154 ms | 669 ms |
| Most memory held | 41.4 MB | **35.1 MB** | 69.3 MB |

## Writing numbers as text

[`bench/format`](../bench/format): each onto a new string, 200,000 doubles from 1 to 1,000 to three places, `{v:.3}`; 200,000 integers below 1,048,576 padded to 8, `{i:>8}`; a million of them, `{i}`; and the 200,000 doubles in their shortest digits, `{v}`. Rust writes with `write!` into a `String`, and Python joins f-strings with `"".join`, the quickest way it has to put many pieces together. All three write the same digits.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **7.56 ms** | 17.0 ms | 36.1 ms |
| 200,000 integers, `{i:>8}` | **1.48 ms** | 3.61 ms | 29.2 ms |
| A million integers, `{i}` | **5.48 ms** | 13.3 ms | 85.1 ms |
| 200,000 doubles, `{v}` | **5.03 ms** | 12.0 ms | 65.8 ms |
| CPU time, the whole run | **156 ms** | 328 ms | 1,688 ms |
| Most memory held | 41.7 MB | **28.6 MB** | 134.6 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| 200,000 doubles, `{v:.3}` | **8.38 ms** | 16.2 ms | 28.4 ms |
| 200,000 integers, `{i:>8}` | **1.55 ms** | 3.90 ms | 25.5 ms |
| A million integers, `{i}` | **5.69 ms** | 13.5 ms | 60.6 ms |
| 200,000 doubles, `{v}` | **5.37 ms** | 11.7 ms | 54.3 ms |
| CPU time, the whole run | **162 ms** | 330 ms | 1,411 ms |
| Most memory held | 39.6 MB | **24.7 MB** | 142.0 MB |

[Printing](reference/printing.md#formats) says how Calx writes them.

## Reading and writing JSON

[`bench/json`](../bench/json): 100,000 player records, 12.6 MB, each with an id, a name, a position, a list of tags, a flag and a level. The text parsed into a tree, the tree written back as compact text, and the text read into typed records. Calx uses [`std.json`](std/json.md) and `@json` structs, Rust serde_json's `Value` and `#[derive(Deserialize)]`, and Python `json` and dataclasses made from its dicts.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **31.3 ms** | 73.6 ms | 113 ms |
| Written back | **20.1 ms** | 24.1 ms | 103 ms |
| Read into typed records | 55.8 ms | **26.2 ms** | 216 ms |
| CPU time, the whole run | **891 ms** | 1,203 ms | 4,156 ms |
| Most memory held | 292.0 MB | **215.4 MB** | 281.2 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| Parsed into a tree | **31.1 ms** | 34.7 ms | 78.2 ms |
| Written back | 21.2 ms | **19.4 ms** | 88.1 ms |
| Read into typed records | 55.2 ms | **18.3 ms** | 102 ms |
| CPU time, the whole run | 928 ms | **757 ms** | 2,412 ms |
| Most memory held | 290.2 MB | **231.5 MB** | 289.5 MB |

- serde reads typed records straight from the text. Calx's `@json` reads them from the tree, which it parses first, so its typed read is the parse and more: 2.1 times serde's time on Windows and 3 times on Linux.
- serde_json's parse takes twice as long on Windows as on Linux, where Calx's takes the same. Rust programs use each system's allocator. On Linux serde_json's parse takes 12% longer than Calx's, and its writing 9% less.

## A tree of files

[`bench/walk`](../bench/walk): 10,000 small files in 420 directories, a third each `.calx`, `.txt` and `.json`, in the temp folder. Everything in the tree walked, each file and directory counted, and the paths `**/*.calx` matches found, with [`std.fs`](std/fs.md)'s `walk` and `glob`, Rust's walkdir and glob crates, and Python's `os.walk` and `glob.glob`. Each sorts the names, as `std.fs` does.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **14.9 ms** | 16.1 ms | 30.5 ms |
| `**/*.calx` found | **15.6 ms** | 17.0 ms | 42.9 ms |
| CPU time, the whole run | **234 ms** | 250 ms | 562 ms |
| Most memory held | 6.0 MB | **4.8 MB** | 13.1 MB |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The tree walked | **1.62 ms** | 2.73 ms | 5.20 ms |
| `**/*.calx` found | **1.87 ms** | 2.56 ms | 7.12 ms |
| CPU time, the whole run | **29.6 ms** | 50.0 ms | 112 ms |
| Most memory held | 3.6 MB | **2.9 MB** | 11.4 MB |

All three take 6 to 9 times as long on Windows as on Linux.

## Starting up

[`bench/start`](../bench/start): the smallest program, which prints a line and ends, timed from outside, the best of 20 runs in each of 5 rounds. Then as a script, the best of 25: changed, built and run, which for Calx is `calx run`, for Rust `rustc`, which builds for debugging as `cargo run` does, and then the program, and for Python `python`; and run again unchanged.

| Windows | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **3.63 ms** | 3.93 ms | 17.4 ms |
| Its CPU time | 0 ms | 0 ms | 15.6 ms |
| Most memory held | **3.9 MB** | 4.2 MB | 10.8 MB |
| Changed, then run | 105 ms | 158 ms | **17.4 ms** |
| Run again, unchanged | 26.7 ms | **4.25 ms** | 17.4 ms |

| Linux | Calx | Rust | Python |
| --- | --- | --- | --- |
| The program, built, run | **0.29 ms** | 0.32 ms | 6.21 ms |
| Its CPU time | **0.26 ms** | 0.28 ms | 6.45 ms |
| Most memory held | 2.9 MB | 2.9 MB | 9.1 MB |
| Changed, then run | 50.6 ms | 148 ms | **6.21 ms** |
| Run again, unchanged | 15.4 ms | **0.35 ms** | 6.21 ms |

- Rust's last row is its program alone, built before; Python reads and compiles the script each time it runs it, so its rows are the same time.
- `calx run` of a script it has built checks that nothing the script was built from has changed, and then runs what it built: 15 to 23 ms more than the program alone.

## What they show

On these benchmarks, Calx takes a quarter to two thirds of Rust's time to take text apart and write numbers, 7 to 41% less to walk a tree of files, and a little less to start. It parses JSON into a tree faster and writes it about as fast, but takes 2 to 3 times as long as serde to read typed records, as `@json` goes through the tree. Python takes 1.4 to 28 times as long as Calx at each step, least where its C does most of the work, as in replacing a word, but it runs a small script, changed or not, sooner than `calx run` does.

Calx held 18 to 60% more memory than Rust on every benchmark but the smallest program.
