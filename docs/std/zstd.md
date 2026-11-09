---
title: std.zstd
description: Zstandard files decompressed as Python's compression.zstd reads them, with checksums, skippable frames and dictionaries, compressed at zstd's levels, with or without a dictionary, on all the processors for big ones, and dictionaries trained from samples.
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
| `train_dict(samples: List<List<u8>>, size: usize) -> Dictionary ! Error` | A dictionary of at most `size` bytes trained from samples of what it's for, as Python's `train_dict(samples, size)` trains one. See [Training one](#training-one) |
| `finalize_dict(content: Dictionary, samples: List<List<u8>>, size: usize, level: i32) -> Dictionary ! Error` | A dictionary with `content`'s bytes as its content and tables from the samples, as Python's `finalize_dict(content, samples, size, level)` makes one |
| `d.id() -> u32` | The ID frames compressed with `d` carry, or 0 |
| `d.to_bytes() -> List<u8>` | The dictionary's bytes, as `dict_content` gives them: to keep a trained one in a file, which `Dictionary.new` reads back |
| `d.decompress(bytes: List<u8>, limit: u64 = u64.MAX) -> List<u8> ! Error` | As `decompress`, with the dictionary, as `decompress(bytes, zstd_dict=d)` |
| `d.decompress_into(bytes: List<u8>, from: usize, to: usize, limit: u64, out: mut List<u8>) ! Error` | As `decompress_into`, with the dictionary |
| `d.compress(bytes: List<u8>, level: i32 = 3, checksum: bool = false) -> List<u8>` | As `compress`, with the dictionary, as `compress(bytes, level, zstd_dict=d)` |
| `d.compress_into(bytes: List<u8>, from: usize, to: usize, level: i32, checksum: bool, out: mut List<u8>)` | As `compress_into`, with the dictionary |

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
| `Training` | `train_dict` was given fewer than seven samples, fewer than 8 bytes in the first three quarters of them, or a size under 256; or `finalize_dict` a size under 256 or under its content's | too few samples or too small a size to train a dictionary | Src size is incorrect, or Destination buffer is too small |

Python raises a `ZstdError` for all but `BadDictionary`, with the message above after "Unable to decompress Zstandard data: ", or for `Training` after "Unable to train the Zstandard dictionary: " or "Unable to finalize the Zstandard dictionary: ".

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

That table is libzstd's for inputs over 256 KiB. A smaller input takes libzstd's settings for inputs of its size, up to 256, 128 or 16 KiB, with a dictionary's content counted in, as libzstd picks them: smaller windows and tables, and the deeper searches at lower levels, so binary trees from level 11, or 9 up to 16 KiB, and the optimal parser from 13, or 11. From level 3 to the last hash-row level, for an input the window holds all of, the long hash table and the rows have no more slots than the input has positions, half what libzstd allows: on our small files after a dictionary that's 3 to 5% faster at levels 3 and 5, for at most 0.13% more bytes, and on 1 MB 7 to 12% faster at 9 and 12 for 0.06%. A longer input takes libzstd's, so level 12 is a deeper search than 11 there.

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
    println("{bytes.len()} {fast.len()} {small.len()}")    // out: 88638 8866 6619
    println("{decompress(small)! == bytes}")    // out: true
}
```

Python writes 8,865 bytes for that text at level 1, and 6,770 at 19 without the checksum's 4.

- **One frame**, with the size it decompresses to, as Python writes it, and with no window size when the input fits in the window, as libzstd leaves it out. A checksum only when `checksum` is set: Python leaves it out by default too, and `zstd` puts one in. A short input comes out as Python writes it, byte for byte; a longer one is a little bigger or smaller than Python's, as [Speed](#speed) shows.
- **Blocks.** The text is cut into blocks of 128 KiB. Each block's literals are Huffman-coded and its sequences' codes FSE-coded, with whichever tables come out smallest: the block's own, the ones the block before used, or the predefined ones. Up to level 5, as libzstd does there, a block of fewer than 1000 sequences with a dictionary's tables to keep takes those or the predefined ones without pricing tables of its own for them ([How it's built](#how-its-built)). A block that doesn't get smaller is stored as it is, and one byte over and over as a run. A block whose text changes partway, as in a tar of text and programs, is cut where the two parts come out smaller apart, from an estimate of each part's size.
- **On all the processors.** An input longer than four windows, 2 MiB at level 1, 8 MiB at level 3 and 32 MiB at 19, is cut into jobs of that size, as `zstd -T0` cuts them, and the jobs compress at once. Each job's matches reach back into the text before it, an eighth of the window up to level 7, half from 8 to 15, and the whole window from 16, and each job starts with tables of its own. On our files the cuts cost at most 0.4% of the size, and under 0.1% at most levels. Text that repeats from further back than a job reaches, but within the window, is found again in each job, as `zstd -T0` finds it: 3 MB of our small samples over and over, every 425 KB, comes out 422 KB at level -5, where libzstd's threads write 417 KB and one job 206 KB.
- **The same bytes make the same frame**, on any machine with any number of processors, with AVX2, with SSE2 alone or with neither: the jobs are cut by size and level alone, and the encoder works in integers. So a `.tar.zst` built twice is the same file.
- Each thread compressing a job needs the level's tables, from 32 KiB at the negative levels to 640 MiB at 22, or less for an input smaller than the window, and room for what its job compresses to. `compress` puts no more threads on the jobs than a quarter of the machine's memory has room for, and writes the same bytes with fewer.
- `compress` panics for a level outside -131072 to 22, where Python raises a `ValueError`. To compress with a dictionary, it's `d.compress`: see [Dictionaries](#dictionaries).

## Dictionaries

Small files compress much better with a dictionary, and need it to decompress. `Dictionary.new` takes one that `zstd --train` or Python's `train_dict` made: it starts with the magic number `37 a4 30 ec` and an ID that isn't 0, then the tables and the content. `Dictionary.raw` takes any bytes as content alone, which matches reach back into, as `ZstdDict(bytes, is_raw=True)` does. `std.zstd` decompresses with both, and compresses with them as Python does with `compress(bytes, level, zstd_dict=d)`.

```calx
import std.bytes.{from_hex}
import std.zstd.{Dictionary, compress}

fn main() {
    // Python's `compress(b"the lazy dog jumps over the quick brown fox. " * 2, zstd_dict=ZstdDict(past, is_raw=True))`.
    let past = "the quick brown fox jumps over the lazy dog. ".repeat(4).to_bytes()
    let d = Dictionary.raw(past)!
    let text = d.decompress(from_hex("28b52ffd205a950000082e0500508de0f5b8a775013304309c0102")!)!
    println("{text.len()} {d.id()}")    // out: 90 0
    // And back: much smaller with the dictionary than without.
    println("{d.compress(text).len()} {compress(text).len()}")    // out: 26 60
}
```

Python writes 27 bytes for that text with the dictionary, and 63 without.

- **Compressing with one**, its content is the text before the input: matches reach back into it from the first byte, as far as the window, which is picked for the input and the content together. A trained dictionary's Huffman and FSE tables are the first block's tables from before, used where they come out smaller, and its three repeated offsets are the first block's. The frame carries the dictionary's ID, in as few bytes as it fits, as Python writes it. Content alone has no ID, so its frames carry none.
- Only an input's first job matches into the dictionary, as with `zstd -T0`: in an input big enough for several, the others match into the text before them.
- On our 1,143 small files, JSON records of about 130 bytes and pieces of the repository's source of 0.3 to 4 KB, `d.compress` with a dictionary Python's `train_dict` made from them comes out 1.5 to 15% smaller than Python's at levels -5 to 5, where libzstd uses a dictionary's tables on a small input without checking they pay, and from 3% smaller to 0.2% bigger from 6 on. With 8.7 KB of them as content alone, it's at most 0.3% bigger from -1 to 10, and 1.4 to 2.1% smaller from 11.
- A dictionary's content goes into the match finder's tables once for each level's settings, and the calls after use those tables rather than take it in again, which on a small input would be most of the work. A trained dictionary's entropy tables are built once too. On those files, with the trained dictionary, that's 1.4 µs a file at level 1, 5.9 µs at 9 and 20 µs at 19, where Python's `compress` takes 37, 43 and 700 µs. A `ZstdCompressor` kept from one file to the next takes 1.6, 7.5 and 36 µs, and at level 5 3.15 µs to std.zstd's 3.2. Up to level 5 it keeps the dictionary's tables on small inputs, where std.zstd takes them or the predefined ones, whichever codes the block in fewer bits, and comes out 1.5 to 15% smaller.
- A frame that carries an ID needs a dictionary with that ID: without one, or with another, it fails with `Dictionary`. A frame without one decompresses with whatever dictionary it's given, as libzstd does, and without one.
- As libzstd reads them, bytes given to `raw` that start with the magic number are a dictionary all the same, with their ID and tables.
- Both fail with `BadDictionary` for fewer than eight bytes, as Python does, and for a dictionary whose tables are broken. Python's `ZstdDict` takes those and raises a `ZstdError` the first time it decompresses with them; `std.zstd` fails at once. `new` also fails for bytes without the magic number or with an ID of 0, as `ZstdDict` does.
- As libzstd's lets them, matches may reach back past a dictionary's content into its tables. `d.compress`'s don't.

### Training one

`train_dict(samples, size)` makes a dictionary from samples of what it's for, as Python's `train_dict` does, by libzstd's fastCover method, written from its description rather than its code. `d.to_bytes()` gives its bytes to keep in a file, which `Dictionary.new` reads back.

```calx
import std.zstd.{compress, train_dict}

fn main() {
    // Records like the ones the dictionary is for.
    var samples: List<List<u8>> = []
    for i in 0..1000 {
        samples.push("\{\"id\": {i * 7919 % 10007}, \"name\": \"user{i * 31 % 977}\", \"admin\": {i % 7 == 0}\}".to_bytes())
    }
    let d = train_dict(samples, 4096)!
    let record = "\{\"id\": 4242, \"name\": \"user42\", \"admin\": false\}".to_bytes()
    let z = d.compress(record)
    println("{z.len()} {compress(record).len()} {d.decompress(z)! == record}")    // out: 27 55 true
    println("{d.to_bytes().len()}")    // out: 4096
}
```

Python's `train_dict` on those samples makes a dictionary that compresses the record to 27 bytes too.

- **What it holds.** The first three quarters of the samples train it and the last quarter tests it. Its content is stretches of the training samples, one from each part of them in turn: the stretch whose 8-byte strings come up most often in all the samples, each counted once, after which they count for nothing, so the next stretch adds what the content doesn't have yet. The stretches fill the content from its end, so the best are nearest the text that follows. Its tables are for what the training samples' parses at level 3 take with that content before them, each literal, length and offset code counted from one, so that every byte can be coded. Five lengths of stretch are tried, 50 to 1,998 bytes, at once on five processors, and the one kept is the one whose dictionary and test samples, a frame each at level 3, come to the fewest bytes.
- **Where it differs from libzstd's.** When the content and the tables are more than `size`, libzstd leaves out the end of the content, where the best stretches are, and `std.zstd` its start. The literals' Huffman code is the shortest of at most 11 bits for their counts: the encoder's own way of fitting a code to 11 bits took 10% more bits than that on those literals, where most bytes never come up but still need a code. The ID is a hash of the content, from 32,768 up as libzstd's are, but not libzstd's hash.
- **Sizes.** On four sets of samples, JSON records, pieces of the repository's source, a server's log in pieces of 3 to 25 lines, and the small files of [Dictionaries](#dictionaries), each trained at 4 KB, 16 KB and 110 KB, with the samples held out compressed a frame each at levels 1, 3, 9 and 19: compressed by Python, they come out from 1.5% smaller to 1.8% bigger with `std.zstd`'s dictionary than with Python's, 0.1% bigger on average, and over 1% bigger in one case of 48. Compressed by `std.zstd`, they come out from 2.5% smaller to 1.1% bigger, 0.5% smaller on average. The same content made into a dictionary by both `finalize_dict`s comes out within 1.2% either way compressed by Python, and 0.4% compressed by `std.zstd`.
- **Speed.** On an i9-13900K on Windows, training from those sets, 0.3 to 3.3 MB, at 16 KB and 110 KB, took 9 to 34 ms, where Python's took 25 to 166 ms: 2.9 to 5.2 times as fast. Picking the content takes most of it, and is about 3 ns a byte of samples for each length of stretch.
- The same samples give the same dictionary, on any machine with any number of processors.
- `finalize_dict(content, samples, size, level)` puts tables on content picked some other way, from all the samples' parses at `level`, as Python's does. When the content and the tables don't fit in `size`, it keeps the content's end, where Python keeps its start. As in Python, `size` can't be less than the content.
- `train_dict` fails with `Training` for fewer than seven samples, as five must be left to train on, for fewer than 8 bytes in the first three quarters of them, and for a size under 256; `finalize_dict` for a size under 256 or under the content's. Python raises a `ZstdError` for each.

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
| -1 | 31.6 ms | 35.9 ms | 33.7 ms | 6,308,516 | 6,306,781 |
| 1 | 33.0 ms | 37.4 ms | 34.9 ms | 5,344,041 | 5,347,922 |
| 2 | 38.0 ms | 41.3 ms | 38.3 ms | 5,078,735 | 5,078,988 |
| 3 | 45.7 ms | 48.4 ms | 46.3 ms | 4,767,836 | 4,753,525 |
| 4 | 50.9 ms | 52.9 ms | 51.7 ms | 4,739,407 | 4,723,978 |
| 5 | 86.0 ms | 98.3 ms | 91.1 ms | 4,428,239 | 4,455,092 |
| 6 | 126 ms | 139 ms | 129 ms | 4,261,547 | 4,282,647 |
| 7 | 146 ms | 160 ms | 151 ms | 4,177,269 | 4,202,757 |
| 8 | 189 ms | 206 ms | 195 ms | 4,125,301 | 4,152,012 |
| 9 | 198 ms | 221 ms | 204 ms | 4,090,200 | 4,120,710 |
| 10 | 273 ms | 328 ms | 322 ms | 4,028,337 | 4,067,118 |
| 11 | 344 ms | 480 ms | 467 ms | 3,989,559 | 4,035,545 |
| 12 | 395 ms | 525 ms | 508 ms | 3,986,131 | 4,033,230 |
| 13 | 1,243 ms | 1,318 ms | 1,223 ms | 3,978,445 | 3,985,152 |
| 14 | 1,630 ms | 1,702 ms | 1,658 ms | 3,938,160 | 3,944,077 |
| 15 | 2,013 ms | 2,373 ms | 2,285 ms | 3,912,317 | 3,917,800 |
| 16 | 2,542 ms | 2,828 ms | 2,719 ms | 3,700,122 | 3,704,873 |
| 17 | 2,956 ms | 3,614 ms | 3,316 ms | 3,631,993 | 3,647,661 |
| 18 | 3,194 ms | 4,505 ms | 4,221 ms | 3,625,404 | 3,613,289 |
| 19 | 3,816 ms | 5,398 ms | 5,060 ms | 3,602,926 | 3,593,426 |
| 20 | 3,968 ms | 5,587 ms | 5,240 ms | 3,427,231 | 3,420,656 |
| 21 | 4,054 ms | 6,118 ms | 5,710 ms | 3,424,774 | 3,418,755 |
| 22 | 4,236 ms | 6,346 ms | 6,072 ms | 3,424,844 | 3,418,181 |

`clang40` on one thread:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 | Calx's size | libzstd's size |
| --- | --- | --- | --- | --- | --- |
| 1 | 69.7 ms | 87.6 ms | 77.4 ms | 20,265,378 | 20,256,662 |
| 3 | 126 ms | 133 ms | 123 ms | 17,629,230 | 17,556,416 |
| 5 | 228 ms | 257 ms | 241 ms | 16,682,422 | 16,654,722 |
| 9 | 551 ms | 572 ms | 540 ms | 15,490,602 | 15,518,610 |
| 13 | 3,148 ms | 3,570 ms | 3,410 ms | 15,174,079 | 15,293,478 |
| 16 | 4,813 ms | 6,808 ms | 6,364 ms | 14,802,311 | 14,828,491 |
| 19 | 11,778 ms | 14,438 ms | 13,705 ms | 13,886,093 | 13,888,852 |
| 22 | 12,714 ms | 18,260 ms | 18,043 ms | 13,611,938 | 13,616,304 |

On all the processors: `std.zstd` as a program runs it, and libzstd with a worker for each of the 32 processors, as `zstd -T0` runs it:

| File | Level | Calx | Python 3.14, 32 workers | Rust: zstd 0.13, 32 workers | Calx's size | libzstd's size |
| --- | --- | --- | --- | --- | --- | --- |
| src.tar | 1 | 5.9 ms | 11.1 ms | 9.9 ms | 5,344,041 | 5,352,731 |
| src.tar | 3 | 22.3 ms | 27.8 ms | 26.5 ms | 4,767,836 | 4,757,456 |
| src.tar | 5 | 39.3 ms | 51.5 ms | 47.9 ms | 4,428,239 | 4,461,129 |
| src.tar | 9 | 182 ms | 212 ms | 201 ms | 4,090,200 | 4,127,791 |
| src.tar | 13 | 1,134 ms | 1,209 ms | 1,193 ms | 3,978,445 | 3,992,533 |
| src.tar | 16 | 2,063 ms | 2,627 ms | 2,526 ms | 3,700,122 | 3,705,752 |
| src.tar | 19 | 3,821 ms | 5,398 ms | 4,993 ms | 3,602,926 | 3,592,062 |
| src.tar | 22 | 4,282 ms | 6,341 ms | 6,099 ms | 3,424,844 | 3,418,367 |
| clang40 | 1 | 9.1 ms | 21.9 ms | 16.0 ms | 20,265,378 | 20,259,344 |
| clang40 | 3 | 30.0 ms | 40.0 ms | 35.3 ms | 17,629,230 | 17,601,328 |
| clang40 | 5 | 51.1 ms | 67.8 ms | 62.9 ms | 16,682,422 | 16,733,050 |
| clang40 | 9 | 264 ms | 290 ms | 276 ms | 15,490,602 | 15,558,830 |
| clang40 | 13 | 1,645 ms | 1,785 ms | 1,754 ms | 15,174,079 | 15,338,409 |
| clang40 | 16 | 2,234 ms | 3,248 ms | 3,071 ms | 14,802,311 | 14,842,998 |
| clang40 | 19 | 8,560 ms | 12,349 ms | 12,184 ms | 13,886,093 | 13,892,293 |
| clang40 | 22 | 12,507 ms | 18,178 ms | 17,506 ms | 13,611,938 | 13,619,791 |

In WSL on the same machine, where Python's libzstd is Ubuntu 26.04's and the crate's is built by GCC, the sizes are the same and the times are these. On one thread, `src.tar`:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 |
| --- | --- | --- | --- |
| -1 | 31.6 ms | 31.5 ms | 31.7 ms |
| 1 | 34.0 ms | 32.5 ms | 32.4 ms |
| 2 | 38.3 ms | 37.3 ms | 37.2 ms |
| 3 | 49.6 ms | 46.9 ms | 47.9 ms |
| 4 | 52.5 ms | 51.7 ms | 52.5 ms |
| 5 | 87.5 ms | 87.3 ms | 85.3 ms |
| 6 | 124 ms | 125 ms | 120 ms |
| 7 | 149 ms | 141 ms | 142 ms |
| 8 | 190 ms | 183 ms | 181 ms |
| 9 | 216 ms | 204 ms | 203 ms |
| 10 | 316 ms | 308 ms | 303 ms |
| 11 | 389 ms | 392 ms | 384 ms |
| 12 | 462 ms | 465 ms | 441 ms |
| 13 | 1,441 ms | 1,436 ms | 1,416 ms |
| 14 | 1,812 ms | 1,886 ms | 1,808 ms |
| 15 | 2,263 ms | 2,669 ms | 2,595 ms |
| 16 | 2,635 ms | 3,078 ms | 2,902 ms |
| 17 | 3,210 ms | 3,818 ms | 3,712 ms |
| 18 | 3,462 ms | 4,788 ms | 4,618 ms |
| 19 | 4,044 ms | 5,408 ms | 5,256 ms |
| 20 | 4,185 ms | 5,536 ms | 5,493 ms |
| 21 | 4,281 ms | 5,972 ms | 5,977 ms |
| 22 | 4,755 ms | 6,047 ms | 6,246 ms |

`clang40` on one thread:

| Level | Calx | Python 3.14 | Rust: zstd 0.13 |
| --- | --- | --- | --- |
| 1 | 69.8 ms | 72.6 ms | 68.3 ms |
| 3 | 132 ms | 128 ms | 121 ms |
| 5 | 232 ms | 231 ms | 226 ms |
| 9 | 546 ms | 530 ms | 522 ms |
| 13 | 3,492 ms | 3,690 ms | 3,711 ms |
| 16 | 5,412 ms | 7,549 ms | 6,803 ms |
| 19 | 12,974 ms | 15,556 ms | 14,910 ms |
| 22 | 14,059 ms | 20,002 ms | 19,707 ms |

On all the processors:

| File | Level | Calx | Python 3.14, 32 workers | Rust: zstd 0.13, 32 workers |
| --- | --- | --- | --- | --- |
| src.tar | 1 | 6.6 ms | 11.8 ms | 10.2 ms |
| src.tar | 3 | 22.7 ms | 28.4 ms | 27.4 ms |
| src.tar | 5 | 41.0 ms | 47.8 ms | 45.5 ms |
| src.tar | 9 | 192 ms | 199 ms | 189 ms |
| src.tar | 13 | 1,252 ms | 1,340 ms | 1,276 ms |
| src.tar | 16 | 2,144 ms | 2,807 ms | 2,725 ms |
| src.tar | 19 | 4,091 ms | 5,159 ms | 5,201 ms |
| src.tar | 22 | 4,747 ms | 6,057 ms | 5,975 ms |
| clang40 | 1 | 9.3 ms | 21.3 ms | 21.2 ms |
| clang40 | 3 | 31.6 ms | 39.4 ms | 37.1 ms |
| clang40 | 5 | 53.6 ms | 64.4 ms | 61.2 ms |
| clang40 | 9 | 275 ms | 285 ms | 268 ms |
| clang40 | 13 | 1,866 ms | 1,942 ms | 1,891 ms |
| clang40 | 16 | 2,423 ms | 3,576 ms | 3,277 ms |
| clang40 | 19 | 9,150 ms | 13,389 ms | 12,732 ms |
| clang40 | 22 | 14,341 ms | 19,875 ms | 19,544 ms |

- Python and the `zstd` crate run libzstd 1.5.7. On Windows both are built by MSVC, and the crate's is 5 to 8% faster. GCC's, in WSL, is faster again: by up to 7% to level 10, and 13 to 18% at 11 and 12. The Rust programs were built as above, and each Rust time is the faster of the two builds' best. Each program checked that what it wrote decompressed to the file.
- `ruzstd` 0.8 compresses only at its level 1, "Fastest": 164 ms and 7,054,586 bytes for `src.tar`, and 457 ms and 23,917,712 bytes for `clang40`, on Windows, and 148 and 408 ms in WSL.
- On one thread, against the faster libzstd, on our two files: on Windows `std.zstd` is 1 to 6% faster on `src.tar` from level -1 to 9, 1.18 to 1.36 times as fast from 10 to 12, 2% slower at 13, and 1.02 to 1.43 times as fast from 14. On `clang40` it's 2% slower at 3 and 9 and faster at the rest. In WSL, against GCC's, it takes up to 6% longer on `src.tar` from -1 to 13, is level at 14, and is 1.1 to 1.4 times as fast from 15; on `clang40` up to 10% longer to level 9, and faster from 13.
- What `std.zstd` writes is at most 0.4% bigger than libzstd's at every level timed, and up to 1.1% smaller. On `src.tar` it's within 0.07% at levels -1 to 2, 0.3% bigger at 3 and 4, 0.5 to 1.2% smaller from 5 to 12, 0.1 to 0.4% smaller from 13 to 17, and 0.2 to 0.3% bigger from 18 to 22. The most is `clang40` at level 3, 0.4% bigger. The sizes are from the encoder as it is now; the times are from before the last changes to it.
- On all the processors, `std.zstd` is 1.5 to 2.3 times as fast as the faster libzstd at level 1, 1.1 to 1.2 times at 3 and 5, and 1.2 to 1.4 times from 16. At 9 and 13 it's 1.05 to 1.1 times as fast on Windows, and within 2% either way in WSL. Both cut the same jobs, four windows long. That's 2 MiB at level 1, so `src.tar` makes 9 jobs. From 9 on the jobs are 16 MiB or more, so the files make 3 at most, and most of the processors wait.
- On many small files, our 1,143 of [Dictionaries](#dictionaries) at one call each, on Windows on a performance core, `compress` takes 2.8 µs a file at level 1, 8.3 µs at 9 and 37 µs at 19. Python's `compress` takes 5.7, 10.6 and 49 µs, and a `ZstdCompressor` kept from one file to the next 3.5, 8.4 and 47 µs.
- On those files without a dictionary, what `compress` writes is smaller than Python's at every level: 0.05 to 0.2% at -5, -1 and 1, 0.35 to 0.5% from 2 to 10, and 0.3 to 0.45% from 11.

## How it's checked

`std.zstd` was run on Python 3.14's `compression.zstd` with libzstd 1.5.7, on Windows and on Linux:

- 90 files: text, random bytes, zeros, runs and a mix, from empty to 2.4 MB, compressed by Python at levels -5, 1, 3, 9 and 19, with checksums, without sizes as a stream writes them, with long-distance matching and a window of 16 MiB, and as two frames with a skippable one between. All gave the same bytes.
- 11 big files, 18.3 MB of source and 40 MB of a program, compressed by libzstd's threads as `zstd -T` compresses, at levels -7, 3, 19 and 22, with jobs of 1 MiB, and with long-distance matching and a window of 128 MiB. All gave the same bytes, on all the processors.
- 4,056 broken files made from those: cut at random and at the headers' edges, with bits flipped, bytes changed, added and removed, and with garbage, a magic number, a header and a skippable frame cut short after them; and frames made by hand with windows from 128 MiB to 8 GiB, sizes around 32 KiB, sizes that don't match, and blocks bigger than their window. All but the 27 above gave the same kind of error, or the same bytes.
- 2,184 pairs of frames and dictionaries: frames compressed by Python with two trained dictionaries and a raw one, with checksums, without IDs, and streamed, each decompressed with no dictionary, the right one, the wrong one, as raw bytes, cut at six places, with bits flipped at seven, with an ID of 0, and of fewer than eight bytes. All gave what Python gave.

The [tests](../../tests/std/zstd.calx) hold Python's bytes and Python's outcomes.

What `compress` writes was checked on 70 files: text, a program, random bytes, zeros, runs, text that repeats every 1 to 70,000 bytes with and without noise, a mix of kinds, and slices of `src.tar` cut a byte either side of the block size and its multiples, from empty to 2.5 MB. Each was compressed at every level from 1 to 22 and at -1, -3, -7 and -50: 1,820 frames, by an encoder built to check every match it takes against the text. Python 3.14's `compression.zstd`, `zstd` 1.5.7 and `std.zstd` each decompressed all 1,820 to what went in. The same files at 18 levels came out the same from the encoder built for AVX2, for SSE2 alone and for neither, as on Arm. The [tests](../../tests/std/zstd_compress.calx) compress at every level and check that what comes back is what went in. They hold Python's bytes for a short text, and hashes of frames that every machine must match, and check that 6 MB compressed on 8 threads and on 1 is the same.

`train_dict` and `finalize_dict` were checked on the four sets of [Training one](#training-one): 24 dictionaries, with 150 of each set's samples held out compressed by `std.zstd` at levels -5, 1, 3, 9 and 19. Python 3.14's `compression.zstd` and `zstd -D` 1.5.7 decompressed all 18,000 frames to what went in, and Python compressed with the dictionaries and read its frames back. The [tests](../../tests/std/zstd_train.calx) train from records and finalize content picked from them, check what comes back, that the same samples give the same bytes on one thread and on eight, and hold hashes of the dictionaries that every machine must match.

What `d.compress` writes was checked on 1,146 files: 1,000 JSON records, 143 pieces of the repository's source, a file of 200 KB and one of 3 MB made of them end to end, and an empty one. Each was compressed at every level from 1 to 22 and at -1 and -5, with a dictionary Python's `train_dict` made from most of them and with 8.7 KB of them as content alone: 55,008 frames. Python 3.14's `compression.zstd` and `zstd -D` 1.5.7, each with the same dictionary, and `std.zstd` decompressed all of them to what went in. The [tests](../../tests/std/zstd_dict_compress.calx) compress with both kinds at every level, check the frames' headers and IDs, and hold hashes of frames that every machine must match.

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
| Hash tables | The search runs a position ahead of itself: the next position's entry is loaded before this one's candidate is compared, so the loads overlap the comparisons. At levels 1 and 2 positions are looked at in pairs, with the repeated offset tried a pair ahead; at 3 and 4 the long table runs ahead, and a short match looks at the next position's long one. On a miss the search moves on by a step that grows the longer it goes without a match; at 3 and 4, once a job has saved bytes, it starts over every 16 KiB, as libzstd's 8 KB pieces start it over: after a stretch of random bytes it had grown so long that most of a repeat after it was skipped, and 3 MB of random bytes with repeats came out 7% bigger than libzstd's, where it's now 0.3% smaller at 3 and 0.07% bigger at 4. One bound per block keeps entries in the window. At 3 and 4, and at 1 and 2 for a window under 512 KiB, that test has no branch: the entry's address is picked with a conditional move, bytes the text rarely has standing in for one out of the window, and the comparison then also asks that the address was the entry's own. While a table fills, which is all of a small input, whether an entry is in is a coin toss. A hash is the top bits of the bytes times libzstd's constant for their length, so the levels with one table, 2 and below, find what libzstd finds. Text that repeats from further back than a table remembers is found only where an entry happens to last, and the parse goes the same way through each repeat: with a constant of our own, 3 MB repeating every 425 KB never found the repeat at -1, 1.15 MB to libzstd's 282 KB. A sequence's literals are copied 16 bytes at a time, with no call in the loop |
| Hash rows | Each row holds the last 15, 31 or 63 positions with its hash, and beside each a byte more of its hash. That byte is compared across the whole row at once, 32 at a time with AVX2 but a row of 16 in 16, so the load stays in the row's cache line, and the text is read only where it's likely to match. The next eight positions' rows are fetched into the cache ahead, so the parse stops 16 bytes before a job's end, and the bytes from there to 8 before it get a plainer search that hashes as it goes: a small input's last bytes often match a dictionary's. The parse is built for each depth and for the 5-byte hash most row levels have. Of a match longer than 384, only the first 96 positions and the last 32 go in, as in libzstd. In a stretch without matches, once the search is moving on more than 8 bytes at a time, the positions it moves over stop going into the rows, as in libzstd 1.5.7: random data takes a third of the time, for a little less found in it later. The rows take every position again from the next match |
| Binary trees | The positions with each hash, sorted by the text after them, so a search goes down one path to the longest matches. At levels 13 to 15 a position goes on its hash's list unsorted, and is sorted into the tree only when a search comes to it, since the parse passes over most positions. At most tree levels the tree holds half the window, as libzstd's does. A search that has found nothing yet compares a position it comes to that the tree no longer holds but the window does, and stops there; libzstd compares every such one. On 8 MB of `clang40` at level 16 that's 1.04% smaller, where libzstd's way is 1.44% smaller for five times the far reads |
| Picking matches | From level 5 to 15, by what they're worth: four times the length, less the bits of the distance, so a match a byte longer but much further back loses |
| The optimal parser | Every match and literal from each place, priced from how often each literal, length and distance has come up, carried over from block to block. Without a dictionary, as in libzstd, the first block's literals start from a 256th of how often each byte comes up in it, plus 1 for each it has and nothing for a byte it doesn't have. After a dictionary, whose matches are cheap, every byte starts with 1 and a 16th of its count: libzstd's way comes out 0.25 to 0.45% bigger there on our small files. After that, as in libzstd, the counts stay as they are from block to block until they add up to more than twice 4,096 for the literals or 2,048 for the rest, and are then shifted down to about that. Without a dictionary, on our small files, a count for every byte made the output 0.25 to 0.5% bigger from level 11, and halving every count at each block 0.6% bigger from 16, where a first parse's counts carry over. A price is worked out where it's looked up, from each count's log and its sum's as they were after the last stretch the parser took, so after each stretch only the counts that changed are taken up again, as libzstd sets its base prices, rather than every price made again. From 19, or 18 or 16 for some smaller inputs, a job's first block is parsed twice, the second time with prices from the first, except after a dictionary, as in libzstd |
| Splitting blocks | A block's sequences cut into pieces of 2 to 32 KiB, smaller at the higher levels, with the counts of their literals and codes summed from the block's start, so any run of pieces' counts is one subtraction. A run is split in two where an estimate of the parts' sizes saves the most, then each part again. A block too short for two pieces, as every small input is, has its counts made once for the sections, without the running sums. The negative levels don't split |
| Huffman | Code lengths by Moffat and Katajainen's method from the counts, sorted by a counting sort, in one pass when they're all below 512, as a small block's are. They're at most as long as libzstd lets them be for that many literals, as low as 7 to 9 bits for a few hundred, up to 11: a shallower code's tree can be shorter by more than its data grows, and on our small files that's 0.16% smaller from level 1 to 12. With libzstd's btultra settings, from level 18, or 16 up to 256 KiB and 13 up to 16 KiB, each limit on a code's length is tried instead, as libzstd does there, from the least with room for the symbols up to the code's own depth, and the code whose tree and data come out smallest is kept. On our small files that's 1 to 1.4% smaller, and 0.3% after a dictionary. A new code isn't made for fewer than 32 literals, nor when the last block's, or a dictionary's, can't lose to it: no code takes fewer bits than the literals' entropy, and a tree takes 2 bytes at least. From 6 literals, as in libzstd, the last block's code or a dictionary's codes them if it has every one and comes out smaller than raw: after a trained dictionary most small inputs have fewer than 32, and on our small files that's 0.4 to 1.1% smaller from level 1 to 11. The tree's weights are FSE-coded at accuracies 5 and 6, where libzstd tries one: the bytes each takes are worked out from its states alone, and only the smaller one's stream is written. Four streams written at once, side by side |
| FSE | Probabilities rounded, then nudged where they cost least, at two accuracies: libzstd's for that many codes, about 2 bits under their number's, and the one above. For a few thousand codes or more those are the two largest; for a few hundred, 6 and 7 rather than 8 and 9, whose descriptions took more than their bits saved, and on our small files that's 0.2% smaller at every level. The better is kept if it beats the predefined table, one symbol repeated, and the last block's. Below the lazy levels, as libzstd picks there, one symbol is that symbol repeated; with a dictionary's table to keep and fewer than 1000 codes, it's kept or the predefined one taken, whichever costs less, and none of the block's own is made; a table the block before made is priced with the rest, as libzstd never keeps one there; and with none to keep, the predefined one is taken for fewer codes than a table of their own pays for, 28 offsets to 72 lengths, or for counts spread too evenly. libzstd keeps a dictionary's for any it codes: on our small files after a trained dictionary that's 8 to 24% bigger than pricing every table, and this 2 to 4% bigger, for a call 10 to 25% faster from level -1 to 5. A table spread from counts of 1 or more, as every table the encoder makes is, is laid out 8 slots a time and spread with no slot skipped. Each table keeps what each code costs with it, so pricing one is a loop without branches. A table of the block's own isn't made when no table could cost less than the codes' entropy plus the shortest description one could have, which after a dictionary is most of the time on small inputs, nor at an accuracy where the counts rounded to it leave too many slots for each count's description to be short enough; the bytes are the same. Each sequence's bits go into the stream in two writes, from a loop in a function of its own, so its states and bits stay in registers |
| All the processors | Jobs of four windows, each on a thread of [`parallel for`](../reference/parallel.md), joined in order |
| Small frames | A frame of up to 256 KiB takes a job kept from the frames before, with its buffers and tables, rather than allocating and freeing a job's ten blocks for each call. Up to 16 are kept, from the system's allocator as they last as long as the program, and a kept job's tables are let go when they're over 1 MiB. A block's Huffman code and FSE tables stay where they were made, in one of the job's two places for each kind, so a block that keeps them takes only where they are, rather than copying 7 KiB; a job keeps a copy of the last trained dictionary's tables, so its next frame with that dictionary starts from them where they are |
| Dictionaries | The content is the first job's text before it. It goes into the match finder's tables once for each level's settings, and up to 16 such sets, 32 MiB in all, are kept for the frames after; the least recently used is let go first. Each `Dictionary` has a number of its own, and as its content can't change, a set is found by that and its settings, without comparing the content. At the hash-table and hash-row levels a frame takes a working copy of the set's tables, which holds the content too, and an input of up to 64 KiB goes in after it, so neither the content nor the tables are copied for the frame. When it's done, the frame puts back from the set only the entries its own text changed; frames at once take copies of their own. At the levels with two hash tables and with hash rows, for tables of 256 KiB or more, too big to stay in the cache beside the copy, the frame notes what each of its writes replaced, and puts that back, newest first, without reading the set again; smaller ones are copied back from the set. At the tree levels an input of up to 64 KiB doesn't touch the set's trees: its positions go into trees of its own, sized for it, and each position the parse searches looks in the set's too, which stay as they are, as libzstd attaches a dictionary. Most positions the parse passes over, and those go only into the frame's trees, which stay in the cache; frames at once share the set's. The first frame with a dictionary at those levels builds and keeps its trees before it starts, so it comes out as the frames after do. A longer input copies the set's trees and goes into them. A trained dictionary's Huffman and FSE tables, built from its header, are kept for the last 4 dictionaries. In the binary trees the content's comparisons stop at its end, and text that agrees to there goes in with nothing below it, as in libzstd, so the trees are the same whatever text follows. At the btlazy2 levels it goes in sorted |
| Training | Each 8-byte string's count, by the top 20 bits of its hash, in a table of 2^20, as libzstd's fastCover counts them. A stretch is picked by sliding a window over its part of the samples and keeping the one whose strings' counts, each counted once, add up to most; each string's hash is worked out once, into a ring, and its counts fetched 32 strings ahead, as most of the time went on waiting for the tables, 6 MiB of them, which took a third off it. The tables come from the encoder's own parses: a frame with a place to count into counts its literals and codes and writes nothing. The five lengths of stretch are trained at once, and each one's frames one after another: on more threads, frames this small only wait on one another, and took 1.2 to 2.5 times as long |
