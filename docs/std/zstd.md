---
title: std.zstd
description: Zstandard files decompressed as Python's compression.zstd reads them, with checksums, skippable frames and dictionaries, and compressed at zstd's levels, on all the processors for big ones.
---

# `std.zstd`

`std.zstd` decompresses Zstandard files, such as release archives in `.tar.zst` and what `zstd` and Python's `compression.zstd` write. It gives the same bytes as Python 3.14's `compression.zstd.decompress`, and on broken files it fails where Python fails, with the same kind of error but for one case, below. It also compresses, at `zstd`'s levels, into frames that `zstd` and Python read: see [Compressing](#compressing). Big frames are decoded, and big inputs compressed, on all the processors. [`std.tar`](tar.md) opens and writes a `.tar.zst` with it.

```calx
import std.bytes.{from_hex}
import std.zstd.{Error, decompress, is_zstd}

fn main() {
    // What Python's `compression.zstd.compress(b"hello, zstd\n")` gives.
    let packed = from_hex("28b52ffd200c61000068656c6c6f2c207a7374640a")!
    println("{is_zstd(packed)}")    // out: true
    print(String.from_utf8(decompress(packed.copy())!) ?? "bad")    // out: hello, zstd

    // A limit on what comes out, for bytes that came from elsewhere.
    match decompress(packed, 5) {
        ok(_) => println("fits"),
        err(Error.TooLong) => println("too long"),    // out: too long
        err(e) => println("{e}"),
    }
    match decompress("not zstd".to_bytes()) {
        ok(_) => println("?"),
        err(e) => println("{e}"),    // out: not a Zstandard file
    }
}
```

| Item | Gives |
| --- | --- |
| `decompress(bytes: List<u8>, limit: u64 = u64.MAX) -> List<u8> ! Error` | What the bytes decompress to, every frame in them one after another, as `compression.zstd.decompress(bytes)` gives it. Fails with `TooLong` if that's more than `limit` bytes, which is no limit unless it's given |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, limit: u64, out: mut List<u8>) ! Error` | Decompresses `bytes[from..to]` onto the end of `out`, at most `limit` bytes. On an error, `out` is as it was. Panics unless the range is in `bytes` |
| `compress(bytes: List<u8>, level: i32 = 3, checksum: bool = false) -> List<u8>` | The bytes compressed into one frame, as `compression.zstd.compress(bytes, level)` writes one, with a checksum if `checksum` is set. Panics for a level outside -131072 to 22. See [Compressing](#compressing) |
| `compress_into(bytes: List<u8>, from: usize, to: usize, level: i32, checksum: bool, out: mut List<u8>)` | Compresses `bytes[from..to]` onto the end of `out`, as `compress` does. Panics unless the range is in `bytes` |
| `is_zstd(bytes: List<u8>) -> bool` | Whether the bytes start as a Zstandard frame does: `28 b5 2f fd` |
| `xxh64(bytes: List<u8>, seed: u64 = 0) -> u64` | The XXH64 hash, as the `xxhash` package's `xxh64_intdigest` gives it. A frame's checksum is its low 32 bits |
| `Dictionary.new(bytes: own List<u8>) -> Dictionary ! Error` | A dictionary that `zstd --train` or `train_dict` made, as `ZstdDict(bytes)` takes it. See [Dictionaries](#dictionaries) |
| `Dictionary.raw(bytes: own List<u8>) -> Dictionary ! Error` | Bytes to use as a dictionary as they are, as `ZstdDict(bytes, is_raw=True)` takes them |
| `d.id() -> u32` | The ID frames compressed with `d` carry, or 0 |
| `d.decompress(bytes: List<u8>, limit: u64 = u64.MAX) -> List<u8> ! Error` | As `decompress`, with the dictionary, as `decompress(bytes, zstd_dict=d)` |
| `d.decompress_into(bytes: List<u8>, from: usize, to: usize, limit: u64, out: mut List<u8>) ! Error` | As `decompress_into`, with the dictionary |

Why bytes don't decompress, as `Error`:

| Error | When | `{e}` | Python says |
| --- | --- | --- | --- |
| `Format` | They don't start as a frame does, or bytes after a frame don't | not a Zstandard file | Unknown frame descriptor |
| `Unsupported` | A frame header sets the bit the format reserves | a frame setting that isn't supported | Unsupported frame parameter |
| `Window` | A frame asks for a window of more than 128 MiB | a window bigger than 128 MiB | Frame requires too much memory for decoding |
| `Corrupt` | A block, its tables, its sequences or a frame's size are wrong | corrupt data | Data corruption detected, or what's wrong |
| `Checksum` | A frame's checksum doesn't match what it decompressed to | checksum mismatch | Restored data doesn't match checksum |
| `Dictionary` | A frame needs a dictionary it wasn't given, or another one | the wrong dictionary, or none | Dictionary mismatch |
| `Truncated` | They end inside a frame, or are empty | the data ends early | Compressed data ended before the end-of-stream marker was reached |
| `TooLong` | What they decompress to is more than the limit | longer than the limit when decompressed | |
| `BadDictionary` | `Dictionary.new` or `raw` was given bytes that aren't a dictionary | not a Zstandard dictionary | `ValueError`, or a `ZstdError` when it's first used |

Python raises a `ZstdError` for all but `BadDictionary`, with the message above after "Unable to decompress Zstandard data: ".

- Frames follow one another to the end of the bytes, as `zstd` writes them with `zstd -c a b`. Skippable frames give nothing. After a frame, anything that isn't the start of another fails with `Format`, and the start of one cut short with `Truncated`, as in Python. An empty list is `Truncated` too.
- A frame's size, when it gives one, must be what its blocks make. A checksum, when it has one, is checked.
- Windows bigger than 128 MiB fail as Python's do, since `compression.zstd` keeps libzstd's limit of 2^27. libzstd skips that check for a frame it decodes in one go: one that's all there and says it makes at most 32 KiB, the room Python's output starts with. `std.zstd` skips it for the same frames.
- The limit counts what comes out, so a few bytes that would decompress to gigabytes fail without the memory. Python has no limit for `decompress`; `std.zstd` has none by default either.
- One difference: Huffman-coded literals in four streams must each end where the stream does, as RFC 8878 asks. libzstd's fast decoder on x86-64 only counts what comes out of them, so for a few broken files Python gives wrong bytes, or a checksum mismatch, where `std.zstd` says `Corrupt`. In the 4,056 broken files below, that was 27.

## Compressing

`compress` takes `zstd`'s levels, as Python's `compression.zstd.compress` does: 1, the fastest, to 22, the smallest, with 0 for the default, 3, and negative levels down to -131072 for faster still. `zstd` asks for `--ultra` past 19; Python and `std.zstd` don't. Each level has the settings libzstd gives it, so a frame needs as much memory to decompress as `zstd`'s at the same level, and the search is about as thorough:

| Level | Window | Matches found with | Parser |
| --- | --- | --- | --- |
| -131072 to -1 | 512 KiB | A hash table, looking at fewer places the lower the level | The first match, with the literals left uncoded, so they decompress fast too |
| 1, 2 | 512 KiB, 1 MiB | A hash table | The first match |
| 3, 4 | 2 MiB | Two hash tables, of 8 bytes and of 5 | The first match, or a longer one at the next place |
| 5 | 2 MiB | Rows of 16 in a hash table | The best match |
| 6, 7 | 2 MiB | Rows of 16 | The best match, or a better one a place on |
| 8 | 2 MiB | Rows of 16 | The best, or a better one one or two places on |
| 9 to 12 | 4 MiB | Rows of 16, 32, 64 and 64 | The same |
| 13 to 15 | 4 MiB | Binary trees, 16, 32 and 64 deep, sorted as they're searched | The same |
| 16, 17 | 4 MiB, 8 MiB | Binary trees, 32 deep | Optimal |
| 18 | 8 MiB | Binary trees, 64 deep, and matches of 3 bytes | Optimal |
| 19 to 22 | 8, 32, 64 and 128 MiB | Binary trees, 128 deep, or 512 at 22, and matches of 3 bytes | Optimal, with prices from a first parse |

The optimal parser prices every way through the next few thousand bytes, from how often each literal, length and distance has come up, and takes the cheapest. A window is never bigger than the input needs.

```calx
import std.zstd.{compress, decompress}

fn main() {
    var text = String()
    for i in 0..2000 {
        text.push_str("{i}: {i * i % 977} green bottles, hanging on the wall\n")
    }
    let bytes = text.to_bytes()
    let fast = compress(bytes, 1)
    let small = compress(bytes, 19, true)
    println("{bytes.len()} {fast.len()} {small.len()}")    // out: 88638 8616 6689
    println("{decompress(small)! == bytes}")    // out: true
}
```

Python writes 8,865 bytes for that text at level 1, and 6,770 at 19 without the checksum's 4.

- **One frame**, with the size it decompresses to, as Python writes it, and with no window size when the input fits in the window, as libzstd leaves it out. A checksum only when `checksum` is set: Python leaves it out by default too, and `zstd` puts one in. A short input comes out as Python writes it, byte for byte; a longer one is a little bigger or smaller than Python's, as [Speed](#speed) shows.
- **Blocks.** The text is cut into blocks of 128 KiB. Each block's literals are Huffman-coded and its sequences' codes FSE-coded, with whichever tables come out smallest: the block's own, the ones the block before used, or the predefined ones. A block that doesn't get smaller is stored as it is, and one byte over and over as a run. A block whose text changes partway, as in a tar of text and programs, is cut where the two parts come out smaller apart, from an estimate of each part's size.
- **On all the processors.** An input longer than four windows, 2 MiB at level 1, 8 MiB at level 3 and 32 MiB at 19, is cut into jobs of that size, as `zstd -T0` cuts them, and the jobs compress at once. Each job's matches reach back into the text before it, an eighth of the window up to level 7, half from 8 to 15, and the whole window from 16, and each job starts with tables of its own. On our files the cuts cost at most 0.4% of the size, and under 0.1% at most levels.
- **The same bytes make the same frame**, on any machine with any number of processors, with AVX2, with SSE2 alone or with neither: the jobs are cut by size and level alone, and the encoder works in integers. So a `.tar.zst` built twice is the same file.
- Each thread compressing a job needs the level's tables, from 32 KiB at the negative levels to 640 MiB at 22, or less for an input smaller than the window, and room for what its job compresses to. `compress` puts no more threads on the jobs than a quarter of the machine's memory has room for, and writes the same bytes with fewer.
- `compress` panics for a level outside -131072 to 22, where Python raises a `ValueError`. It doesn't take a dictionary yet.

## Dictionaries

Small files compress much better with a dictionary, and need it to decompress. `Dictionary.new` takes one that `zstd --train` or Python's `train_dict` made: it starts with the magic number `37 a4 30 ec` and an ID that isn't 0, then the tables and the content. `Dictionary.raw` takes any bytes as content alone, which matches reach back into, as `ZstdDict(bytes, is_raw=True)` does. `std.zstd` decompresses with them; it doesn't compress with them yet.

```calx
import std.bytes.{from_hex}
import std.zstd.{Dictionary}

fn main() {
    // Python's `compress(b"the lazy dog jumps over the quick brown fox. " * 2, zstd_dict=ZstdDict(past, is_raw=True))`.
    let past = "the quick brown fox jumps over the lazy dog. ".repeat(4).to_bytes()
    let d = Dictionary.raw(past)!
    let text = d.decompress(from_hex("28b52ffd205a950000082e0500508de0f5b8a775013304309c0102")!)!
    println("{text.len()} {d.id()}")    // out: 90 0
}
```

- A frame that carries an ID needs a dictionary with that ID: without one, or with another, it fails with `Dictionary`. A frame without one decompresses with whatever dictionary it's given, as libzstd does, and without one.
- As libzstd reads them, bytes given to `raw` that start with the magic number are a dictionary all the same, with their ID and tables.
- Both fail with `BadDictionary` for fewer than eight bytes, as Python does, and for a dictionary whose tables are broken. Python's `ZstdDict` takes those and raises a `ZstdError` the first time it decompresses with them; `std.zstd` fails at once. `new` also fails for bytes without the magic number or with an ID of 0, as `ZstdDict` does.
- As libzstd's lets them, matches may reach back past a dictionary's content into its tables.

## Speed

`std.zstd` reserves the room a frame says it makes all at once and decodes into it. A frame of 1 MiB or more, or one without a size, is decoded on all the processors with [`parallel for`](../reference/parallel.md)'s threads: while one run of blocks has its literals and sequences decoded on every processor, the run before it has its matches carried out, in order, and the checksum is worked on over what's done. A frame that's smaller is decoded on one thread.

On an i9-13900K, best of 7 runs in each of three rounds, the programs taking turns. On Windows the ones on one thread ran on the performance cores, out of power throttling; WSL can't be held to them, so its numbers are rougher. `src.tar` is 18.3 MB of the Calx repository's files and `clang40` the first 40 MB of `clang.exe`, compressed by Python at level 3, its default, and level 19. On Windows:

| File | Calx | Calx, one thread | Python 3.14 | Rust: zstd 0.13 | Rust: ruzstd 0.8 |
| --- | --- | --- | --- | --- | --- |
| src.tar.3.zst | 5.5 ms | 7.8 ms | 19.7 ms | 14.3 ms | 39.3 ms |
| src.tar.19.zst | 6.0 ms | 7.6 ms | 20.7 ms | 14.2 ms | 36.8 ms |
| clang40.3.zst | 14.7 ms | 23.8 ms | 49.8 ms | 40.1 ms | 127.1 ms |
| clang40.19.zst | 21.3 ms | 32.7 ms | 61.4 ms | 47.2 ms | 137.9 ms |

In WSL on the same machine:

| File | Calx | Calx, one thread | Python 3.14 | Rust: zstd 0.13 | Rust: ruzstd 0.8 |
| --- | --- | --- | --- | --- | --- |
| src.tar.3.zst | 6.0 ms | 8.1 ms | 11.7 ms | 8.3 ms | 33.9 ms |
| src.tar.19.zst | 6.2 ms | 7.7 ms | 11.7 ms | 8.1 ms | 29.8 ms |
| clang40.3.zst | 15.5 ms | 24.3 ms | 50.4 ms | 34.9 ms | 126.3 ms |
| clang40.19.zst | 23.6 ms | 32.2 ms | 56.2 ms | 39.8 ms | 139.1 ms |

- Python and the `zstd` crate run libzstd 1.5.7 on one thread. On Windows both are built by MSVC, which leaves out libzstd's assembly Huffman decoder; in WSL, Python's is Ubuntu 26.04's and the crate's is built by GCC, both with it. `ruzstd` is written in Rust.
- "Calx, one thread" is the same decoder with its threads left out, to compare the work each does; a program always gets all the processors for a frame of 1 MiB or more.
- On one thread, `std.zstd` is a little ahead of GCC's libzstd on text, 8.1 ms to 8.3 on src.tar.3 and 7.7 to 8.1 on src.tar.19, and 1.2 to 1.4 times as fast on clang40. On Windows, against MSVC's, it's 1.4 to 1.9 times as fast.
- The Rust programs were built with `lto = "fat"` and one codegen unit, once as they are and once with `target-cpu=x86-64-v3`, as Calx builds for x86-64-v3, and in WSL with libzstd built for it too. Each Rust time is the faster of the two builds' best. Each had the room for its output reserved before it started, and checked that both crates gave the same bytes.

`compress` was timed on the same machine with little else running, `std.zstd`, Python and the Rust programs taking turns for three rounds, best of 3 runs in each up to level 9 and of 1 from 10, on the same two files before they were compressed. On one thread, each program on the performance cores and out of power throttling, `src.tar` at every level, on Windows:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 | Calx's size | libzstd's size |
| --- | --- | --- | --- | --- | --- |
| -1 | 34.5 ms | 36.1 ms | 33.7 ms | 6,309,762 | 6,306,781 |
| 1 | 36.2 ms | 37.2 ms | 34.8 ms | 5,357,880 | 5,347,922 |
| 2 | 40.4 ms | 41.2 ms | 38.4 ms | 5,089,301 | 5,078,988 |
| 3 | 47.8 ms | 48.8 ms | 46.1 ms | 4,800,345 | 4,753,525 |
| 4 | 54.7 ms | 52.2 ms | 50.5 ms | 4,773,542 | 4,723,978 |
| 5 | 99.9 ms | 96.5 ms | 90.6 ms | 4,428,574 | 4,455,092 |
| 6 | 138 ms | 139 ms | 129 ms | 4,261,751 | 4,282,647 |
| 7 | 157 ms | 161 ms | 150 ms | 4,177,473 | 4,202,757 |
| 8 | 205 ms | 206 ms | 196 ms | 4,124,699 | 4,152,012 |
| 9 | 221 ms | 222 ms | 209 ms | 4,089,446 | 4,120,710 |
| 10 | 302 ms | 333 ms | 313 ms | 4,027,116 | 4,067,118 |
| 11 | 400 ms | 475 ms | 461 ms | 3,989,297 | 4,035,545 |
| 12 | 435 ms | 531 ms | 509 ms | 3,985,765 | 4,033,230 |
| 13 | 1,285 ms | 1,304 ms | 1,291 ms | 3,984,004 | 3,985,152 |
| 14 | 1,660 ms | 1,702 ms | 1,691 ms | 3,943,434 | 3,944,077 |
| 15 | 2,020 ms | 2,415 ms | 2,316 ms | 3,912,630 | 3,917,800 |
| 16 | 2,550 ms | 2,851 ms | 2,602 ms | 3,714,554 | 3,704,873 |
| 17 | 3,079 ms | 3,725 ms | 3,388 ms | 3,642,793 | 3,647,661 |
| 18 | 3,172 ms | 4,610 ms | 4,306 ms | 3,636,289 | 3,613,289 |
| 19 | 3,766 ms | 5,355 ms | 5,059 ms | 3,604,786 | 3,593,426 |
| 20 | 4,090 ms | 5,477 ms | 5,111 ms | 3,428,953 | 3,420,656 |
| 21 | 4,116 ms | 6,100 ms | 5,708 ms | 3,426,683 | 3,418,755 |
| 22 | 4,220 ms | 6,226 ms | 5,939 ms | 3,425,473 | 3,418,181 |

`clang40` on one thread:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 | Calx's size | libzstd's size |
| --- | --- | --- | --- | --- | --- |
| 1 | 78.0 ms | 85.0 ms | 75.9 ms | 20,417,622 | 20,256,662 |
| 3 | 127 ms | 129 ms | 119 ms | 17,647,408 | 17,556,416 |
| 5 | 258 ms | 250 ms | 236 ms | 16,682,499 | 16,654,722 |
| 9 | 562 ms | 573 ms | 531 ms | 15,490,195 | 15,518,610 |
| 13 | 3,433 ms | 3,554 ms | 3,613 ms | 15,281,703 | 15,293,478 |
| 16 | 4,777 ms | 7,059 ms | 6,690 ms | 15,019,394 | 14,828,491 |
| 19 | 11,757 ms | 14,791 ms | 14,271 ms | 13,890,651 | 13,888,852 |
| 22 | 12,854 ms | 18,468 ms | 17,837 ms | 13,617,004 | 13,616,304 |

On all the processors: `std.zstd` as a program runs it, and libzstd with a worker for each of the 32 processors, as `zstd -T0` runs it:

| File | Level | Calx | Python 3.14, 32 workers | Rust: zstd 0.13, 32 workers | Calx's size | libzstd's size |
| --- | --- | --- | --- | --- | --- | --- |
| src.tar | 1 | 6.1 ms | 11.3 ms | 9.7 ms | 5,357,880 | 5,352,731 |
| src.tar | 3 | 22.6 ms | 27.8 ms | 26.2 ms | 4,800,345 | 4,757,456 |
| src.tar | 5 | 46.0 ms | 50.5 ms | 47.1 ms | 4,428,574 | 4,461,129 |
| src.tar | 9 | 196 ms | 210 ms | 196 ms | 4,089,446 | 4,127,791 |
| src.tar | 13 | 1,163 ms | 1,235 ms | 1,219 ms | 3,984,004 | 3,992,533 |
| src.tar | 16 | 2,011 ms | 2,642 ms | 2,484 ms | 3,714,554 | 3,705,752 |
| src.tar | 19 | 3,787 ms | 5,420 ms | 5,096 ms | 3,604,786 | 3,592,062 |
| src.tar | 22 | 4,436 ms | 6,313 ms | 6,116 ms | 3,425,473 | 3,418,367 |
| clang40 | 1 | 10.0 ms | 23.2 ms | 16.3 ms | 20,417,622 | 20,259,344 |
| clang40 | 3 | 31.5 ms | 39.9 ms | 35.8 ms | 17,647,408 | 17,601,328 |
| clang40 | 5 | 59.8 ms | 67.7 ms | 63.4 ms | 16,682,499 | 16,733,050 |
| clang40 | 9 | 282 ms | 296 ms | 275 ms | 15,490,195 | 15,558,830 |
| clang40 | 13 | 1,767 ms | 1,910 ms | 1,768 ms | 15,281,703 | 15,338,409 |
| clang40 | 16 | 2,264 ms | 3,331 ms | 3,181 ms | 15,019,394 | 14,842,998 |
| clang40 | 19 | 8,558 ms | 12,437 ms | 11,699 ms | 13,890,651 | 13,892,293 |
| clang40 | 22 | 12,804 ms | 18,517 ms | 17,908 ms | 13,617,004 | 13,619,791 |

In WSL on the same machine, where Python's libzstd is Ubuntu 26.04's and the crate's is built by GCC, the sizes are the same and the times are these. On one thread, `src.tar`:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 |
| --- | --- | --- | --- |
| -1 | 35.0 ms | 31.6 ms | 31.7 ms |
| 1 | 36.1 ms | 32.3 ms | 32.7 ms |
| 2 | 40.0 ms | 37.0 ms | 37.0 ms |
| 3 | 50.2 ms | 45.8 ms | 47.3 ms |
| 4 | 57.4 ms | 50.4 ms | 51.1 ms |
| 5 | 99.8 ms | 86.3 ms | 84.6 ms |
| 6 | 135 ms | 121 ms | 119 ms |
| 7 | 160 ms | 139 ms | 139 ms |
| 8 | 205 ms | 180 ms | 179 ms |
| 9 | 231 ms | 202 ms | 204 ms |
| 10 | 337 ms | 302 ms | 290 ms |
| 11 | 426 ms | 392 ms | 376 ms |
| 12 | 482 ms | 439 ms | 432 ms |
| 13 | 1,375 ms | 1,420 ms | 1,397 ms |
| 14 | 1,763 ms | 1,911 ms | 1,871 ms |
| 15 | 2,270 ms | 2,660 ms | 2,639 ms |
| 16 | 2,739 ms | 3,020 ms | 3,000 ms |
| 17 | 3,185 ms | 3,935 ms | 3,897 ms |
| 18 | 3,608 ms | 4,802 ms | 4,519 ms |
| 19 | 4,063 ms | 5,395 ms | 5,252 ms |
| 20 | 4,361 ms | 5,746 ms | 5,349 ms |
| 21 | 4,360 ms | 6,040 ms | 5,890 ms |
| 22 | 4,684 ms | 6,216 ms | 6,034 ms |

`clang40` on one thread:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 |
| --- | --- | --- | --- |
| 1 | 77.5 ms | 72.2 ms | 67.9 ms |
| 3 | 132 ms | 124 ms | 118 ms |
| 5 | 259 ms | 230 ms | 221 ms |
| 9 | 580 ms | 520 ms | 513 ms |
| 13 | 3,427 ms | 3,792 ms | 3,632 ms |
| 16 | 4,873 ms | 7,439 ms | 6,865 ms |
| 19 | 12,561 ms | 15,729 ms | 15,074 ms |
| 22 | 14,200 ms | 20,330 ms | 19,696 ms |

On all the processors:

| File | Level | Calx | Python 3.14, 32 workers | Rust: zstd 0.13, 32 workers |
| --- | --- | --- | --- | --- |
| src.tar | 1 | 6.5 ms | 11.8 ms | 11.2 ms |
| src.tar | 3 | 24.0 ms | 28.8 ms | 27.7 ms |
| src.tar | 5 | 47.8 ms | 47.3 ms | 46.2 ms |
| src.tar | 9 | 214 ms | 196 ms | 190 ms |
| src.tar | 13 | 1,311 ms | 1,350 ms | 1,263 ms |
| src.tar | 16 | 2,142 ms | 2,847 ms | 2,728 ms |
| src.tar | 19 | 4,032 ms | 5,113 ms | 5,148 ms |
| src.tar | 22 | 4,603 ms | 6,053 ms | 6,082 ms |
| clang40 | 1 | 10.4 ms | 23.1 ms | 22.3 ms |
| clang40 | 3 | 32.1 ms | 41.9 ms | 37.8 ms |
| clang40 | 5 | 63.4 ms | 63.9 ms | 60.3 ms |
| clang40 | 9 | 296 ms | 284 ms | 271 ms |
| clang40 | 13 | 1,818 ms | 2,037 ms | 1,846 ms |
| clang40 | 16 | 2,418 ms | 3,472 ms | 3,347 ms |
| clang40 | 19 | 9,204 ms | 13,282 ms | 12,674 ms |
| clang40 | 22 | 14,723 ms | 20,430 ms | 19,753 ms |

- Python and the `zstd` crate run libzstd 1.5.7. On Windows both are built by MSVC, and the crate's is about 5% faster. GCC's, in WSL, is faster again: by up to 8% to level 10, and 18 to 22% at 11 and 12. The Rust programs were built as above, and each Rust time is the faster of the two builds' best. Each program checked that what it wrote decompressed to the file.
- `ruzstd` 0.8 compresses only at its level 1, "Fastest": 164 ms and 7,054,586 bytes for `src.tar`, and 444 ms and 23,917,712 bytes for `clang40`, on Windows, and 148 and 403 ms in WSL.
- On one thread, against the faster libzstd, `std.zstd` takes longer up to level 9 and is as fast or faster from 10. On Windows it takes 2 to 10% longer from -1 to 9, and it's 1.04 to 1.17 times as fast at 10 to 12, level at 13 and 14, and as fast or faster from 15, up to 1.4 times. In WSL it takes 8 to 18% longer from -1 to 12, is level at 13, and is 1.06 to 1.35 times as fast from 14. On `clang40` it's the same: slower up to 9, faster from 13.
- What `std.zstd` writes is within 1.3% of libzstd's size at every level timed. On `src.tar` it's 0.2 to 1.1% bigger at levels 1 to 4, 0.5 to 1.2% smaller from 5 to 12, within 0.3% from 13 to 17, and 0.2 to 0.6% bigger from 18 to 22. The most is `clang40` at level 16, 1.3% bigger.
- On all the processors, `std.zstd` is 1.6 to 2.1 times as fast as the faster libzstd at level 1, about 1.15 times at 3, and 1.2 to 1.4 times from 16. From 5 to 13 it's within 6% either way on Windows, and up to 13% slower in WSL. Both cut the same jobs, four windows long. That's 2 MiB at level 1, so `src.tar` makes 9 jobs. From 9 on the jobs are 16 MiB or more, so the files make 3 at most, and most of the processors wait.

## How it's checked

`std.zstd` was run on Python 3.14's `compression.zstd` with libzstd 1.5.7, on Windows and on Linux:

- 90 files: text, random bytes, zeros, runs and a mix, from empty to 2.4 MB, compressed by Python at levels -5, 1, 3, 9 and 19, with checksums, without sizes as a stream writes them, with long-distance matching and a window of 16 MiB, and as two frames with a skippable one between. All gave the same bytes.
- 11 big files, 18.3 MB of source and 40 MB of a program, compressed by libzstd's threads as `zstd -T` compresses, at levels -7, 3, 19 and 22, with jobs of 1 MiB, and with long-distance matching and a window of 128 MiB. All gave the same bytes, on all the processors.
- 4,056 broken files made from those: cut at random and at the headers' edges, with bits flipped, bytes changed, added and removed, and with garbage, a magic number, a header and a skippable frame cut short after them; and frames made by hand with windows from 128 MiB to 8 GiB, sizes around 32 KiB, sizes that don't match, and blocks bigger than their window. All but the 27 above gave the same kind of error, or the same bytes.
- 2,184 pairs of frames and dictionaries: frames compressed by Python with two trained dictionaries and a raw one, with checksums, without IDs, and streamed, each decompressed with no dictionary, the right one, the wrong one, as raw bytes, cut at six places, with bits flipped at seven, with an ID of 0, and of fewer than eight bytes. All gave what Python gave.

The [tests](../../tests/std/zstd.calx) hold Python's bytes and Python's outcomes.

What `compress` writes was checked on 70 files: text, a program, random bytes, zeros, runs, text that repeats every 1 to 70,000 bytes with and without noise, a mix of kinds, and slices of `src.tar` cut a byte either side of the block size and its multiples, from empty to 2.5 MB. Each was compressed at every level from 1 to 22 and at -1, -3, -7 and -50: 1,820 frames, by an encoder built to check every match it takes against the text. Python 3.14's `compression.zstd`, `zstd` 1.5.7 and `std.zstd` each decompressed all 1,820 to what went in. The same files at 18 levels came out the same from the encoder built for AVX2, for SSE2 alone and for neither, as on Arm. The [tests](../../tests/std/zstd_compress.calx) compress at every level and check that what comes back is what went in. They hold Python's bytes for a short text, and hashes of frames that every machine must match, and check that 6 MB compressed on 8 threads and on 1 is the same.

## How it's built

Decompressing is [runtime/calx_zstd.c](../../runtime/calx_zstd.c), linked only into programs that call it, written from RFC 8878 and the XXH64 specification, with no libzstd code. The frames are read in Calx over it: [std/zstd.calx](../../std/zstd.calx).

| Part | How |
| --- | --- |
| Bits | Read backwards 64 at a time, from the end of a stream to its start. A sequence takes one reload of the bits when its extra bits come to 30 or fewer, which most do, and counts the bits it has left rather than those it's read, so each read is a subtraction, a shift and a mask |
| Huffman | Tables of up to 2^12 entries, two bytes each. Four streams are decoded together, four symbols from each between reloads |
| FSE | The three tables a block's sequences use, built from their counts as RFC 8878 spreads them, or the predefined ones, or kept from the block before |
| Literals and matches | Copied 32 bytes at a time when the match is 32 or more back, 16 when it's 16 or more, and 8 at a time after its first 8 bytes when it repeats a shorter pattern. The output keeps 64 bytes of room past the end for these |
| XXH64 | Four lanes, 32 bytes a step |
| All the processors | For a frame of 1 MiB or more, or without a size: the next run of blocks' headers and tables are read in order while the run before has its literals and sequences decoded on every processor, and the one before that has its matches carried out in order, with the offsets worked out 8 sequences ahead so their bytes are fetched first, while the checksum is worked on over what's done |

Compressing is [runtime/calx_zstdenc.c](../../runtime/calx_zstdenc.c), linked only into programs that compress, written from RFC 8878. It has no libzstd code, but it has libzstd's table of each level's settings, whose BSD notice is in the file.

| Part | How |
| --- | --- |
| Hash tables | At levels 1 and 2, two positions looked up at once, so the loads overlap. At 3 and 4, the next position's lookups are started before the current match is worked out |
| Hash rows | Each row holds the last 15, 31 or 63 positions with its hash, and beside each a byte more of its hash. That byte is compared across the whole row at once, 32 at a time with AVX2, so the text is read only where it's likely to match. The next eight positions' rows are fetched into the cache ahead |
| Binary trees | The positions with each hash, sorted by the text after them, so a search goes down one path to the longest matches. At levels 13 to 15 a position goes on its hash's list unsorted, and is sorted into the tree only when a search comes to it, since the parse passes over most positions |
| Picking matches | From level 5 to 15, by what they're worth: four times the length, less the bits of the distance, so a match a byte longer but much further back loses |
| The optimal parser | Every match and literal from each place, priced from how often each literal, length and distance has come up, carried over from block to block and halved at each. From 19, a job's first block is parsed twice, the second time with prices from the first |
| Splitting blocks | A block's sequences cut into pieces of 2 to 32 KiB, smaller at the higher levels, with the counts of their literals and codes summed from the block's start, so any run of pieces' counts is one subtraction. A run is split in two where an estimate of the parts' sizes saves the most, then each part again. The negative levels don't split |
| Huffman | Code lengths of at most 11 bits, by Moffat and Katajainen's method from the counts, sorted by a radix sort. Four streams written at once, side by side |
| FSE | Probabilities rounded, then nudged where they cost least, at the two largest accuracies; kept if they beat the predefined table, one symbol repeated, and the last block's. Each sequence's bits go into the stream in two writes |
| All the processors | Jobs of four windows, each on a thread of [`parallel for`](../reference/parallel.md), joined in order |
