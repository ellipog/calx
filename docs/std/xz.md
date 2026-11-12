---
title: std.xz
description: .xz, .lzma and .lz files decompressed as Python's lzma.decompress reads them, with every filter and check, and .xz and .lzma files compressed as xz and lzma.compress write them, both on all the processors, whole or a piece at a time.
---

# `std.xz`

`std.xz` decompresses .xz files, such as release archives in `.tar.xz`, and the older .lzma files and lzip's .lz files. It reads what `xz` and Python's `lzma` write, and it decompresses the same bytes as Python's `lzma.decompress` does. It fails on the same broken files, for the same reasons.

It compresses too, at `xz`'s presets, into files that `xz` and Python read. On our benchmark they come out within about one percent of liblzma's size, and at the default preset a file of tens of megabytes is written 8 to 13 times as fast as liblzma writes it, on all the processors. [`std.tar`](tar.md) opens a `.tar.xz` with it, and writes one.

Both also take a file [a piece at a time](#a-piece-at-a-time), for a file bigger than memory or bytes from a pipe, and give the same results.

```calx
import std.bytes.{from_hex}
import std.xz.{Error, Format, decompress, is_xz}

fn main() {
    // What Python's `lzma.compress(b"hello, xz\n")` gives.
    let packed = from_hex("fd377a585a000004e6d6b4460200210116000000742fe5a301000968656c6c6f2c20787a0a000000eb12b68bfc71df280001220a151ae1671fb6f37d010000000004595a")!
    println("{is_xz(packed)}")    // out: true
    print(String.from_utf8(decompress(packed.copy())!) ?? "bad")    // out: hello, xz

    // A limit on what comes out, for bytes that came from elsewhere.
    match decompress(packed, Format.Auto, 5) {
        ok(_) => println("fits"),
        err(Error.TooLong) => println("too long"),    // out: too long
        err(e) => println("{e}"),
    }
    match decompress("not xz".to_bytes()) {
        ok(_) => println("?"),
        err(e) => println("{e}"),    // out: not an .xz or .lzma file
    }
}
```

```calx
import std.xz.{Check, Format, compress, decompress}

fn main() {
    let text = "a line of text, and the same line again\n".repeat(1000).to_bytes()
    let packed = compress(text.copy())
    println("{text.len()} bytes in {packed.len()}")    // out: 40000 bytes in 176
    println("{decompress(packed)! == text}")    // out: true

    // Smallest, with SHA-256 checks; fastest; and a .lzma file.
    let small = compress(text.copy(), 9, Check.Sha256)
    let fast = compress(text.copy(), 0)
    let lzma = compress(text.copy(), 6, Check.Crc64, Format.Alone)
    println("{small.len()} {fast.len()} {lzma.len()}")    // out: 200 176 121
}
```

| Item | Gives |
| --- | --- |
| `decompress(bytes: List<u8>, format: Format = Format.Auto, limit: u64 = u64.MAX) -> List<u8> ! Error` | What the bytes decompress to, as `lzma.decompress(bytes)` gives it. Fails with `TooLong` if that's more than `limit` bytes, which is no limit unless it's given |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, format: Format, limit: u64, out: mut List<u8>) ! Error` | Decompresses `bytes[from..to]` onto the end of `out`, at most `limit` bytes. On an error, `out` is as it was. Panics unless the range is in `bytes` |
| `compress(bytes: List<u8>, preset: u32 = 6, check: Check = Check.Crc64, format: Format = Format.Xz, extreme: bool = false, block_size: u64 = 0) -> List<u8>` | The bytes compressed, as `lzma.compress(bytes, preset=...)` and `xz -6` compress them. See [Compressing](#compressing) |
| `Decompressor.new(format: Format = Format.Auto, limit: u64 = u64.MAX) -> Decompressor` | A decompressor that takes the bytes a piece at a time. See [A piece at a time](#a-piece-at-a-time) |
| `Compressor.new(preset: u32 = 6, check: Check = Check.Crc64, format: Format = Format.Xz, extreme: bool = false, block_size: u64 = 0) -> Compressor` | A compressor that takes the text a piece at a time, and writes what `compress` writes |
| `is_xz(bytes: List<u8>) -> bool` | Whether the bytes start as an .xz file does: `fd 37 7a 58 5a 00` |
| `crc64(bytes: List<u8>) -> u64` | The CRC-64 .xz files check blocks with: ECMA-182's, reflected, as `lzma_crc64` gives it |

Which files `decompress` takes, and `compress` writes, as `Format`:

| Format | Takes | Writes | Python's |
| --- | --- | --- | --- |
| `Auto` | .xz, .lzma and .lz files, told apart by how they start | `compress` panics | `FORMAT_AUTO` |
| `Xz` | .xz files only | An .xz file | `FORMAT_XZ` |
| `Alone` | .lzma files only, with any dictionary size in the header | A .lzma file | `FORMAT_ALONE` |

Why bytes don't decompress, as `Error`:

| Error | When | `{e}` |
| --- | --- | --- |
| `Format` | They don't start as a file of the format, or of any format for `Auto` | not an .xz or .lzma file |
| `Options` | A filter, a check or a header flag that the format reserves, or settings no encoder writes | a filter or setting that isn't supported |
| `Corrupt` | The headers, the index or the compressed data are wrong | corrupt data |
| `Checksum` | A block's CRC-32, CRC-64 or SHA-256, or an .lz member's CRC-32, doesn't match what it decompressed to | checksum mismatch |
| `Truncated` | They end before the file does | the data ends early |
| `TooLong` | What they decompress to is more than the limit | longer than the limit when decompressed |

Python's `lzma.decompress` raises an `LZMAError` for each: "Input format not supported by decoder", "Invalid or unsupported options", "Corrupt input data", and "Compressed data ended before the end-of-stream marker was reached" for `Truncated`, which a file from `lzma.open` raises as an `EOFError`. A wrong check is "Corrupt input data" in Python. `std.xz` says which it is.

- Files one after another decompress one after another, as `xz` writes them with `xz -c a b`, and they don't have to be of one format. After the first file, `decompress` stops quietly at bytes that aren't the start of another, as Python does. It fails if they are the start of one, cut short. Null bytes after an .xz stream, padding in fours, are the start of one.
- Every filter .xz has: LZMA2 last, and up to three of delta, x86, PowerPC, IA-64, ARM, ARM-Thumb, ARM64, SPARC and RISC-V before it, with their start offsets. Every check: none, CRC-32, CRC-64 and SHA-256. Checks of the IDs the format reserves for later aren't checked, as liblzma doesn't check them.
- `Auto` takes only the .lzma headers LZMA's tools write, as liblzma's auto decoder does: dictionary sizes of 2^n or 2^n + 2^(n-1), and sizes under 256 GiB, since so much else would pass for one. `Alone` takes any header.
- An .lz file's members are version 0 or 1, and their CRC-32, size and member size are all checked, as liblzma reads them since xz 5.4.
- The limit counts what comes out, so a few bytes that would decompress to gigabytes fail without the memory. Python has no limit for `lzma.decompress`; `std.xz` has none by default either.

## Compressing

`compress` takes `xz`'s presets, which Python's `lzma.compress` takes too:

| Preset | Dictionary | Matches found with | Parser |
| --- | --- | --- | --- |
| 0 | 256 KiB | Hash chains, 4 deep | Fast |
| 1 | 1 MiB | Hash chains, 8 deep | Fast |
| 2 | 2 MiB | Hash chains, 24 deep | Fast |
| 3 | 4 MiB | Hash chains, 48 deep | Fast |
| 4 | 4 MiB | Binary trees, matches of 16 taken at once | Optimal |
| 5 | 8 MiB | Binary trees, 32 | Optimal |
| 6 | 8 MiB | Binary trees, 64 | Optimal |
| 7, 8, 9 | 16, 32 and 64 MiB | Binary trees, 64 | Optimal |

The fast parser takes the longest match unless a repeat of a recent distance is nearly as long, a shorter match is much nearer, or the next byte starts a better one. The optimal parser prices every way through the next few thousand bytes, from the coder's own probabilities, and takes the cheapest. `extreme`, which is `xz -e` and Python's `PRESET_EXTREME`, uses the optimal parser and binary trees at every preset, with matches of 273 taken at once and trees searched 512 deep, or of 192 for presets 3 and 5.

- **`Format.Xz`** writes one .xz stream. There's a block for every `block_size` bytes, or for three times the dictionary, at least 1 MiB, as `xz -T0` cuts them. Each block holds LZMA2 with `lc=3`, `lp=0` and `pb=2`, then its check. Each block's header gives both of its sizes, so `std.xz` and `xz -T` decompress the blocks at once. A block's header asks for no more dictionary than the block is long, so a small file needs little memory to decompress.
- **`Format.Alone`** writes a .lzma file with the same settings: the dictionary and the size in the header, then one LZMA stream without an end marker. It has no check, so `check` makes no difference to it. Python writes the size as unknown and ends the stream with a marker, which takes 5 bytes more. Python, `xz` and `std.xz` read both. The one stream is parsed on one thread, but for presets 4 to 9, and with `extreme`, all the processors find its matches, as below, a window of 1 MiB at a time, while the parse goes through the windows before.
- **Slices.** Each block is cut into slices of 1 MiB, which compress at once. Each slice starts with a state reset, which LZMA2 has for this: the coder's probabilities start afresh, but the text before the slice stays in the dictionary, so matches still reach back across the cut. On our benchmark the resets cost about 0.2% of the size.
- **Matches on all the processors.** For presets 4 to 9, finding matches in the binary trees takes two thirds of the time. Each tree holds only the positions whose first four bytes hash to it, and they go in in order, so the trees are shared out among the threads by hash. Each thread walks its own trees over the block and notes the matches it finds. Then the slices are parsed at once from the notes. They're the matches one thread would find. It takes more memory than one thread, as [Speed](#speed) shows. Presets 0 to 3 give each slice hash chains of its own instead, since those take in the dictionary before the slice quickly.
- **The same bytes make the same file**, on any machine with any number of processors. The slices are cut by size, and each finds the same matches however it's run. So a `.tar.xz` built twice is the same file.
- **Short matches**, of two and three bytes, reach back 256 KiB at most, as further back they seldom pay for their distance. That's what lets a slice find the same ones from the text just before it as one thread finds from all of it.
- `compress` panics for a preset past 9, and for `Format.Auto`.

On one thread, `xz` and liblzma write the whole text as one block without slices, as Python does. So what they write is up to about one percent smaller than what `std.xz` writes, and it decompresses on one thread only.

## A piece at a time

`Decompressor` takes a file a piece at a time, for a file bigger than memory, or bytes from a pipe or a socket, as Python's `LZMADecompressor` and `lzma.open` take them. `Compressor` takes text a piece at a time, as `LZMACompressor` does, and writes the same bytes `compress` writes for all of it.

```calx
import std.xz.{Compressor, Decompressor, compress}

fn main() {
    let text = "a line of text, and the same line again\n".repeat(100000).to_bytes()
    let packed = compress(text.copy())

    // In pieces of 64 KiB, as a file or a pipe would give them, onto one list.
    var d = Decompressor.new()
    var out = List<u8>()
    var at: usize = 0
    while at < packed.len() {
        let end = if at + 65536 < packed.len() { at + 65536 } else { packed.len() }
        d.add_into(packed, at, end, u64.MAX, mut out)!
        at = end
    }
    d.finish_into(mut out)!
    println("{out == text}")    // out: true

    // The text in four pieces, compressed as it comes: the same bytes `compress` gives.
    var c = Compressor.new()
    var made = List<u8>()
    for k in 0usize..4 {
        made.extend(c.add(text[k * 1000000..(k + 1) * 1000000]))
    }
    made.extend(c.finish())
    println("{made == packed}")    // out: true
}
```

A file decompressed into another, a mebibyte at a time, holding neither:

```calx
import std.fs.{append, open_read, read, remove_file, temp_dir, write}
import std.xz.{Decompressor, compress}

fn main() {
    let dir = temp_dir()!
    let text = "some text, many times over\n".repeat(200000).to_bytes()
    write("{dir}/calx_xz_doc.txt.xz", compress(text.copy()))!

    let file = open_read("{dir}/calx_xz_doc.txt.xz")!
    write("{dir}/calx_xz_doc.txt", [])!
    var d = Decompressor.new()
    var at: u64 = 0
    loop {
        let piece = file.read_part(at, 1048576)!
        if piece.is_empty() {
            break
        }
        at += u64(piece.len())
        append("{dir}/calx_xz_doc.txt", d.add(piece)!)!
    }
    append("{dir}/calx_xz_doc.txt", d.finish()!)!
    file.close()!
    println("{read("{dir}/calx_xz_doc.txt")! == text}")    // out: true
    remove_file("{dir}/calx_xz_doc.txt")!
    remove_file("{dir}/calx_xz_doc.txt.xz")!
}
```

| Item | Gives | Python's |
| --- | --- | --- |
| `Decompressor.new(format: Format = Format.Auto, limit: u64 = u64.MAX) -> Decompressor` | A decompressor for `format`, which fails with `TooLong` once more than `limit` bytes have come out | `LZMADecompressor(format)` |
| `d.add(bytes: List<u8>, most: u64 = u64.MAX) -> List<u8> ! Error` | Takes the next piece, of any size, and gives what it and the pieces before decompress to that it hasn't given yet, at most `most` bytes | `d.decompress(data, max_length)` |
| `d.add_into(bytes: List<u8>, from: usize, to: usize, most: u64, out: mut List<u8>) ! Error` | Takes `bytes[from..to]` as the next piece, and puts what it gives onto the end of `out`, which can be kept from one piece to the next. On an error, `out` is as it was | |
| `d.needs_input() -> bool` | Whether the last `add` gave all it could, and wants the next piece. It's false when `add` stopped at `most` bytes; then `add` with no bytes gives more | `d.needs_input` |
| `d.unused() -> List<u8>` | The bytes after the end of the data, which aren't another stream, once `add` comes to them | `d.unused_data` |
| `d.finish() -> List<u8> ! Error`, `d.finish_into(out: mut List<u8>) ! Error` | Ends the data, and gives the rest. Fails with `Truncated` if the data ended early | `d.eof` |
| `Compressor.new(preset: u32 = 6, check: Check = Check.Crc64, format: Format = Format.Xz, extreme: bool = false, block_size: u64 = 0) -> Compressor` | A compressor with `compress`'s settings, which panics as `compress` does | `LZMACompressor(format, check, preset)` |
| `c.add(bytes: List<u8>) -> List<u8>` | Takes the next piece of text, and gives what's compressed that it hasn't given: the stream's header first, then blocks as they fill | `c.compress(data)` |
| `c.finish() -> List<u8>` | Compresses the rest of the text, and gives it with the end of the file | `c.flush()` |

- **The same results.** However the bytes come, from a byte at a time to all at once, `Decompressor` gives what `decompress` gives, and fails where it fails, with the same error. It fails as soon as the bytes show the error, but for `Truncated`, which only `finish` can tell. After an error it fails the same way again.
- **What it holds** is the dictionary, the input it hasn't used, and what it hasn't given out. The dictionary is in a window with room for three of it and 2 MiB more, so that the window seldom slides to make room, and moves little when it does. LZMA2 decodes a chunk at a time, once the chunk is all there, and a chunk holds at most 64 KiB of input.
- **Blocks at once.** `xz` writes both of a block's sizes into its header when it compresses on more than one thread, as it does by default since 5.6, and so does `compress`. The decompressor waits for several such blocks, as many as there are processors, up to 256 MiB of what they decompress to, and then decompresses them at once on all the processors, as `xz -T0 -d` does. That holds more: the blocks' input and their output. With `most` below what a block decompresses to, it decompresses them one at a time instead, holding no more than for any other file.
- **Streams one after another** decompress one after another, as with `decompress` and `lzma.open`. Python's `LZMADecompressor` stops at the end of the first, with the rest in `unused_data`. After a stream, bytes that fail before they decompress to anything aren't another, as for `decompress`: the data ends quietly there, and `unused` gives them. A later stream that fails after it has decompressed something fails, where `decompress` stops quietly before it, since what came out of it has been given.
- **The compressor** cuts the text into blocks by size, as `compress` does, so the same text makes the same file whatever the pieces were. It holds the text until as many blocks as there are processors have filled, up to 64 MiB of text, then compresses them at once and gives them out. A .lzma file has no blocks, so with `Format.Alone` it holds all the text until `finish`, and writes what `compress` writes, with the size in the header. Python's `LZMACompressor` writes a .lzma file as it goes, with the size unknown and an end marker.

Decompressing the files from [How it's checked](#how-its-checked), in ms. Each time is the best of 3 rounds of 5 runs, with the programs taking turns, on the i9-13900K with other programs running, so the numbers are rough. `text` is 30 MB of Calx docs and source, changed a little here and there. `bin` is 15 MB of `calx`'s Linux program. `text.mt` is the 30 MB from `xz -T0 --block-size=4MiB`, in 8 blocks. The pieces are 64 KiB:

| File | Calx | Calx, in pieces | Python 3.14 | Python, in pieces |
| --- | --- | --- | --- | --- |
| text, preset 6 (55:1) | 17.3 | 21.2 | 48.0 | 51.5 |
| text, preset 1, SHA-256 | 127.5 | 132.6 | 243.1 | 256.1 |
| bin, x86, preset 1 | 114.3 | 113.3 | 174.2 | 178.8 |
| text, .lzma, preset 1 | 79.6 | 78.6 | 117.3 | 124.5 |
| text.mt | 14.2 | 15.0 | 133.3 | 136.4 |

In WSL on the same machine, with liblzma built by GCC:

| File | Calx | Calx, in pieces | Python 3.14 | Python, in pieces |
| --- | --- | --- | --- | --- |
| text, preset 6 (55:1) | 18.4 | 21.2 | 34.7 | 24.5 |
| text, preset 1, SHA-256 | 127.3 | 129.8 | 166.1 | 161.8 |
| bin, x86, preset 1 | 108.2 | 107.9 | 106.5 | 102.7 |
| text, .lzma, preset 1 | 74.8 | 77.3 | 71.8 | 69.2 |
| text.mt | 12.1 | 12.9 | 85.1 | 71.3 |

- In pieces, the text is copied out of the window into what `add` gives. A whole file doesn't need that copy, as `decompress` writes the text straight where it goes. The copy is most of the difference, and it shows most on text that compresses 55 to 1, where decoding takes least.
- On one thread, GCC's liblzma was 3 to 10% ahead of `std.xz` on the program and on the .lzma file in these runs. Since [CLX-184](https://acta.aaenz.no/aaen-studios/clx#CLX-184) `std.xz` is ahead on a program and level on text ([Speed](#speed)). Python decodes on one thread, so on `text.mt` it takes 6 to 9 times as long.
- In pieces of 1 MiB, `Compressor` took 94 ms where `compress` took 88 for `text` at preset 0, and 1,458 ms where `compress` took 1,381 at preset 6. The bytes were the same.

## Speed

`std.xz` reads the block sizes `xz` writes into each block's header, reserves all the room at once, and writes each block straight into its place. When a file has several blocks with their sizes, as `xz -T` writes them by default, it decodes them on all the processors at once with [`parallel for`](../reference/parallel.md)'s threads.

On an i9-13900K, best of 7 runs in each of three rounds, the programs taking turns. On Windows the ones on one thread ran on the performance cores, out of power throttling; the threaded ones, and `std.xz` on the `mt` files, ran on any. `src.tar` is 18.6 MB of the Calx repository's files and `bin.exe` the Calx compiler, a 6.2 MB Windows program, each compressed by `xz -6`. The `mt` files are `xz -T0 --block-size=2MiB`'s, with 9 and 3 blocks:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: lzma-rust2 0.25 | Rust: lzma-rs 0.3 |
| --- | --- | --- | --- | --- | --- |
| src.tar.xz | 79.7 ms | 127 ms | 122 ms | 116 ms | 199 ms |
| src.tar.mt.xz | 11.7 ms | 135 ms | 23.2 ms | 21.9 ms | 209 ms |
| bin.exe.xz | 31.8 ms | 50.8 ms | 46.9 ms | 48.3 ms | 77.9 ms |
| bin.exe.mt.xz | 13.0 ms | 51.2 ms | 20.3 ms | 19.6 ms | 79.3 ms |

- Python and the liblzma crate run liblzma 5.8, Python on one thread. Each Rust crate's time is from whichever of its decoders, plain or threaded, was faster on the file: the threaded ones on the `mt` files. Each Rust decoder had the room for its output reserved before it started.
- The Rust programs were built with `lto = "fat"`, one codegen unit and `target-cpu=x86-64-v3`, as Calx builds for x86-64-v3.
- liblzma built by GCC, as Linux distributions ship it, is faster than the Windows builds above. In WSL on the same machine, on one CPU, with a src.tar of 20.8 MB and a bin.exe of 7.0 MB, ten rounds taking turns, Python with it decodes src.tar.xz in 93.3 ms at best and 99.9 at the median, to `std.xz`'s 95.0 and 98.0, and bin.exe.xz in 37.4 and 39.0 ms, to `std.xz`'s 36.2 and 37.0 ([CLX-184](https://acta.aaenz.no/aaen-studios/clx#CLX-184)). On the `mt` files it took 79 and 33 ms, and `std.xz` 13 and 14.

`compress` was timed on the same machine, the programs taking turns for two rounds, best of 3 runs in each, or of 2 for the 40 MB file. The ones on one thread ran on the performance cores, out of power throttling; the threaded ones and `std.xz` ran on any. `std.txt` is 1.25 MB of Calx source, `src.tar` the 18.6 MB above, and `clang40` the first 40 MB of `clang.exe`. At preset 6, the default:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: liblzma, threads | Rust: lzma-rust2 0.25 | Rust: lzma-rust2, threads |
| --- | --- | --- | --- | --- | --- | --- |
| std.txt | 87 ms | 233 ms | 268 ms | 269 ms | 272 ms | 273 ms |
| src.tar | 630 ms | 5,250 ms | 5,660 ms | 5,651 ms | 5,722 ms | 5,706 ms |
| clang40 | 1,285 ms | 15,492 ms | 16,243 ms | 10,617 ms | 16,211 ms | 10,551 ms |

At preset 0, the fastest:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: liblzma, threads | Rust: lzma-rust2 0.25 | Rust: lzma-rust2, threads |
| --- | --- | --- | --- | --- | --- | --- |
| std.txt | 18 ms | 28 ms | 33 ms | 29 ms | 27 ms | 24 ms |
| src.tar | 38 ms | 426 ms | 503 ms | 50 ms | 426 ms | 48 ms |
| clang40 | 115 ms | 1,420 ms | 1,622 ms | 127 ms | 1,460 ms | 149 ms |

- Python and the liblzma crate run liblzma 5.8. The threaded encoders had a thread for each of the 32 processors. They give each block a thread, and cut blocks of three times the dictionary, as `xz -T0` does: 24 MiB at preset 6, so `std.txt` and `src.tar` get one thread and `clang40` two, and 1 MiB at preset 0. `std.xz` cuts the same blocks, and puts all the processors on each.
- At preset 6, what `std.xz` writes is 0.4%, 0.6% and 1.1% bigger than what liblzma writes on one thread, for the three files, and 0.4%, 0.6% and 0.3% bigger than what it writes on threads. At preset 0 it's 3.5%, 3.7% and 1.2% smaller than liblzma's, 0.6% to 1.1% bigger than lzma-rust2's on one thread, and the same size as lzma-rust2's on threads.
- At preset 0 the blocks are 1 MiB, so the threaded encoders have blocks for every processor too, and come near `std.xz`'s time. At preset 6, `std.xz` is 2.7 times as fast as the fastest of the others on `std.txt`, 8.3 times on `src.tar` and 8.2 times on `clang40`.
- `xz` 5.8.3, in WSL on the same machine, which can't be held to the performance cores, took 9.5 s for `src.tar` at preset 6, 6.2 s with `-T0`, and 11.0 s for `clang40` with `-T0`.
- On one thread, after `set_workers(1)`, `std.xz` is ahead of liblzma too, on newer files: a src.tar of 20.5 MB, a bin.exe of 6.4 MB and a std.txt of 1.6 MB. At preset 6, pinned to the performance cores, best of 2 runs in each of two rounds, it took 4.91 s, 1.07 s and 280 ms to Python's 5.73 s, 1.25 s and 305 ms. In WSL on one CPU, against GCC's liblzma, 4.70 s, 1.08 s and 267 ms to 5.86 s, 1.32 s and 317 ms ([CLX-180](https://acta.aaenz.no/aaen-studios/clx#CLX-180)).
- As .lzma, one stream that only one thread can parse, the same files at preset 6 took 2.14 s, 640 ms and 181 ms on all the processors, 5.15 s, 1.14 s and 273 ms on one worker, and 5.78 s, 1.31 s and 324 ms in Python. What `std.xz` writes is the same on any number of workers, and within 0.1% of Python's size.
- The Rust programs were built as above, and once more without `target-cpu`, and each Rust time is the faster of the two builds' best. Each program checked that what it wrote decompressed to the file.

Finding the matches on all the processors takes more memory than one thread does. The trees have room for the dictionary and a window of text more, and the threads note a window's matches, packed into 4 bytes each, or 5 for presets 7 to 9 with `extreme`, before the slices are parsed from them. Peak memory, as `/usr/bin/time` gives it in WSL on the same machine:

| File | Preset | Calx | Python 3.14 | `xz -T1` | `xz -T0` |
| --- | --- | --- | --- | --- | --- |
| src.tar | 0 | 68 MiB | 40 MiB | 5 MiB | 69 MiB |
| src.tar | 6 | 427 MiB | 127 MiB | 95 MiB | 116 MiB |
| src.tar | 9 | 604 MiB | 258 MiB | 226 MiB | 247 MiB |
| clang40 | 0 | 171 MiB | 82 MiB | 5 MiB | 125 MiB |
| clang40 | 6 | 407 MiB | 166 MiB | 95 MiB | 239 MiB |
| clang40 | 9 | 857 MiB | 481 MiB | 410 MiB | 460 MiB |

- Calx and Python hold the whole file and what it compresses to, as `compress` and `lzma.compress` take and give them. `xz` reads the file and writes as it goes.
- On a machine with less than twice the memory that finding a block's matches on all the processors would take, `compress` gives each block one thread, as liblzma does, and writes the same bytes.

## How it's checked

`std.xz` was run on 67,772 files, and for each it gave what Python 3.14's `lzma.decompress` with liblzma 5.8.3 gave: the same bytes, or the same kind of error, on Windows and on Linux.

- Text, programs, random bytes and zeros, from empty to 5 MB, compressed by Python at every preset, with every check, filter and chain of filters, and LZMA settings from `lc=0` to `pb=4`. `xz`'s own files: `-T4` blocks, block lists, ARM64 and RISC-V, `--format=lzma`. .lzma and .lz files, lzip versions 0 and 1.
- The other formats' files given to `Xz` and `Alone`.
- Each of those followed by another of another format, by padding of seven lengths up to 30 bytes, by a file cut in half, and by garbage.
- Every cut of nine small files, at every byte.
- 65,400 files with bytes flipped, changed, added, removed, swapped and added at the end, 25,000 of them from `xz -T`, with blocks that decode at once, and 400 from files of up to 800 KB.

The [tests](../../tests/std/xz.calx) hold Python's bytes and Python's outcomes.

`Decompressor` was run on the same 67,772 files, and on 10 one-block files of 6 to 30 MB, with filters, checks, .lzma and .lz, long enough that its window slides many times. Each was given in pieces of 7, 61 and 4,096 bytes, of 1 byte for files up to 3 KB, and all at once; with at most 5, 777 and 1,000 bytes from each `add`; and with limits just under and at what it decompresses to. Each of the 457,138 runs gave what `decompress` gave, the same bytes or the same error, on Windows and on Linux, with 32 workers and with 1. The [tests](../../tests/std/xz_stream.calx) cut Python's files into pieces too.

`Compressor` was run on text from empty to 5 MB, at presets 0, 1 and 6, in blocks of 64 KiB, of 300 KB and of the preset's size, with each check, in pieces of 1,000 bytes to all of it: 489 files, each the same as `compress` wrote.

What `compress` writes was checked on 16 files: text, programs, random bytes, zeros, runs and repeats, from empty to 5 MB. Each was compressed at every preset, at presets 0, 3 and 6 with `extreme`, in blocks of 300 KB and of the preset's size, with every check, and as .lzma: 448 files. `std.xz`, Python 3.14's `lzma` and `xz` 5.8.3 each decompressed all 448 to what went in. Each file also came out the same on 1, 2, 5 and 32 workers. The [tests](../../tests/std/xz_compress.calx) compress at every preset, check that `std.xz` decompresses what they write, and that it's the same on 1 and 3 workers.

## How it's built

Decompressing is [runtime/calx_xz.c](../../runtime/calx_xz.c), linked only into programs that call it, written from the .xz file format's specification and the LZMA algorithm as the public-domain LZMA SDK describes it.

| Part | How |
| --- | --- |
| The range decoder | Kept in registers, with a fast loop while 64 bytes of input are left that doesn't check where the input ends, and a careful one for the rest |
| Bits in trees | Without branches, and with the next level's probabilities loaded before the bit is known. The range, the code, the probability and the next one are each picked by one subtraction's borrow, as liblzma's x86-64 steps do it |
| Literals after a match | Both of the next probabilities loaded at once. Loading only the one the last bit picks, as liblzma does, was 1.6% faster on text and 6.6% slower on a program |
| The properties | The loop is built twice: once with lc=3, lp=0 and pb=2, which every `xz` preset writes, as constants, so it keeps three values fewer in registers and finds a literal's probabilities with a constant mask and shift, and once for any other properties. That made decoding about 1% faster (CLX-184) |
| Copying a match | 16 bytes at a time, with no branch on its length, wherever each 16 reads only text before the match or 16 or more bytes back. That can write up to 15 bytes past the match, which the next symbols write over, but never past where the block's text ends, so blocks next to each other can decode at the same time |
| CRC-64 | Folded with the processor's carry-less multiply, with tables where it's missing: PCLMUL on x86, in four lanes, 64 bytes a step, and PMULL on Arm64, in eight, 128 bytes a step. On an i9-13900K that's about 39 GB/s on bytes in the cache. Over 64 MiB it's about 15 GB/s there, 17 GB/s on GitHub's Neoverse N2 runners and 39 GB/s on their virtual Apple M1, where four lanes did 22, all rough (CLX-288) |
| CRC-32 and SHA-256 | `std.zlib`'s and `std.hash`'s |
| A piece at a time | LZMA2 decodes a chunk once it's all there. LZMA stops between symbols while a symbol's most input is left, then tries to finish from what's there, going back to a copy of its state if that runs out. The window slides what matches can reach to its front: once the text since the dictionary was last reset is longer than they reach, only the low four bits of positions matter, which pick probabilities. The branch filters say where they stopped, and x86's two numbers and delta's last 256 bytes carry over |

Compressing is [runtime/calx_xzenc.c](../../runtime/calx_xzenc.c), written from the same specification and the LZMA SDK's account of LZMA's encoder. It has no liblzma code.

| Part | How |
| --- | --- |
| The range coder | A 64-bit `low`, whose carry goes into the byte held back before it, and the 0xFF bytes after that |
| Prices | A bit's price from a table of -log2 of 128 probabilities, in sixteenths of a bit. The prices of lengths and distances are kept in tables, worked out again as the probabilities move |
| Hash chains and binary trees | Two-, three- and four-byte hashes. The two- and three-byte ones mix the first byte in the same way, so a byte compared tells the rest. Matches are compared 8 bytes at a time. A tree's search waits on memory at each step, so each step fetches both of the node's children and the text at each while its own text is compared, and the four-byte head is fetched two positions ahead. On Linux the trees ask for 2 MiB pages, so their steps seldom miss the TLB as well |
| The optimal parser | The cheapest way through the next few thousand bytes, with literals, matches, repeats, short repeats, and a literal and a repeat after a match or a repeat, as the SDK's parser has them |
| Chunks | LZMA2 chunks of up to 2 MiB of text, closed before their code passes 64 KiB, from a bound on the code a parse can take. A chunk that doesn't compress is written as it is |
| Threads | The binary trees shared out by hash, the slices parsed at once from their notes, and each window of 16 MiB of text, or of the dictionary, matched while the one before is parsed. For .lzma, windows of 1 MiB, and one parse that goes on from window to window, up to the most it can look ahead before the last one matched |

`std.xz` reads the headers, the index and the footer in Calx over those runtime calls, in the order liblzma checks them, so broken files fail the same way, and writes them for `compress`: [std/xz.calx](../../std/xz.calx).
