---
title: std.zstd
description: Zstandard files decompressed as Python's compression.zstd reads them, with checksums, skippable frames and dictionaries, on all the processors for big frames.
---

# `std.zstd`

`std.zstd` decompresses Zstandard files, such as release archives in `.tar.zst` and what `zstd` and Python's `compression.zstd` write. It gives the same bytes as Python 3.14's `compression.zstd.decompress`, and on broken files it fails where Python fails, with the same kind of error but for one case, below. Big frames are decoded on all the processors. [`std.tar`](tar.md) opens a `.tar.zst` with it.

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

## Dictionaries

Small files compress much better with a dictionary, and need it to decompress. `Dictionary.new` takes one that `zstd --train` or Python's `train_dict` made: it starts with the magic number `37 a4 30 ec` and an ID that isn't 0, then the tables and the content. `Dictionary.raw` takes any bytes as content alone, which matches reach back into, as `ZstdDict(bytes, is_raw=True)` does.

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

On an i9-13900K running other programs, best of 7 runs in each of three rounds, so the numbers are rough. `src.tar` is 18.3 MB of the Calx repository's files and `clang40` the first 40 MB of `clang.exe`, compressed by Python at level 3, its default, and level 19. On Windows:

| File | Calx | Calx, one thread | Python 3.14 | Rust: zstd 0.13 | Rust: ruzstd 0.8 |
| --- | --- | --- | --- | --- | --- |
| src.tar.3.zst | 6.8 ms | 10.5 ms | 22.7 ms | 16.3 ms | 42.4 ms |
| src.tar.19.zst | 7.3 ms | 11.5 ms | 22.4 ms | 17.2 ms | 40.7 ms |
| clang40.3.zst | 18.0 ms | 31.3 ms | 53.9 ms | 46.5 ms | 139.7 ms |
| clang40.19.zst | 28.8 ms | 49.2 ms | 71.2 ms | 63.0 ms | 168.0 ms |

In WSL on the same machine:

| File | Calx | Calx, one thread | Python 3.14 | Rust: zstd 0.13 | Rust: ruzstd 0.8 |
| --- | --- | --- | --- | --- | --- |
| src.tar.3.zst | 7.2 ms | 10.5 ms | 12.0 ms | 8.8 ms | 36.3 ms |
| src.tar.19.zst | 7.5 ms | 10.0 ms | 12.5 ms | 8.9 ms | 32.0 ms |
| clang40.3.zst | 18.8 ms | 30.3 ms | 52.2 ms | 35.4 ms | 138.4 ms |
| clang40.19.zst | 30.8 ms | 45.2 ms | 65.8 ms | 48.7 ms | 152.0 ms |

- Python and the `zstd` crate run libzstd 1.5.7 on one thread. On Windows both are built by MSVC, which leaves out libzstd's assembly Huffman decoder; in WSL, Python's is Ubuntu 26.04's and the crate's is built by GCC, both with it. `ruzstd` is written in Rust.
- "Calx, one thread" is the same decoder with its threads left out, to compare the work each does; a program always gets all the processors for a frame of 1 MiB or more.
- On one thread, GCC's libzstd is faster than `std.zstd` on text: 8.8 ms to 10.5 on src.tar.3, and 8.9 to 10.0 on src.tar.19. On clang40 it's the other way round, and on Windows `std.zstd` is faster on all four.
- The Rust programs were built with `lto = "fat"` and one codegen unit, once as they are and once with `target-cpu=x86-64-v3`, as Calx builds for x86-64-v3, and in WSL with libzstd built for it too. Each Rust time is the faster of the two builds' best. Each had the room for its output reserved before it started, and checked that both crates gave the same bytes.

## How it's checked

`std.zstd` was run on Python 3.14's `compression.zstd` with libzstd 1.5.7, on Windows and on Linux:

- 90 files: text, random bytes, zeros, runs and a mix, from empty to 2.4 MB, compressed by Python at levels -5, 1, 3, 9 and 19, with checksums, without sizes as a stream writes them, with long-distance matching and a window of 16 MiB, and as two frames with a skippable one between. All gave the same bytes.
- 11 big files, 18.3 MB of source and 40 MB of a program, compressed by libzstd's threads as `zstd -T` compresses, at levels -7, 3, 19 and 22, with jobs of 1 MiB, and with long-distance matching and a window of 128 MiB. All gave the same bytes, on all the processors.
- 4,056 broken files made from those: cut at random and at the headers' edges, with bits flipped, bytes changed, added and removed, and with garbage, a magic number, a header and a skippable frame cut short after them; and frames made by hand with windows from 128 MiB to 8 GiB, sizes around 32 KiB, sizes that don't match, and blocks bigger than their window. All but the 27 above gave the same kind of error, or the same bytes.
- 2,184 pairs of frames and dictionaries: frames compressed by Python with two trained dictionaries and a raw one, with checksums, without IDs, and streamed, each decompressed with no dictionary, the right one, the wrong one, as raw bytes, cut at six places, with bits flipped at seven, with an ID of 0, and of fewer than eight bytes. All gave what Python gave.

The [tests](../../tests/std/zstd.calx) hold Python's bytes and Python's outcomes.

## How it's built

`std.zstd` is [runtime/calx_zstd.c](../../runtime/calx_zstd.c), linked only into programs that call it, written from RFC 8878 and the XXH64 specification, with no libzstd code. The frames are read in Calx over it: [std/zstd.calx](../../std/zstd.calx).

| Part | How |
| --- | --- |
| Bits | Read backwards 64 at a time, from the end of a stream to its start. A sequence takes one reload of the bits when its extra bits come to 30 or fewer, which most do |
| Huffman | Tables of up to 2^12 entries, two bytes each. Four streams are decoded together, four symbols from each between reloads |
| FSE | The three tables a block's sequences use, built from their counts as RFC 8878 spreads them, or the predefined ones, or kept from the block before |
| Literals and matches | Copied 32 bytes at a time when the match is 32 or more back, 16 when it's 16 or more, and 8 at a time after its first 8 bytes when it repeats a shorter pattern. The output keeps 64 bytes of room past the end for these |
| XXH64 | Four lanes, 32 bytes a step |
| All the processors | For a frame of 1 MiB or more, or without a size: the next run of blocks' headers and tables are read in order while the run before has its literals and sequences decoded on every processor, and the one before that has its matches carried out in order, with the offsets worked out 8 sequences ahead so their bytes are fetched first, while the checksum is worked on over what's done |
