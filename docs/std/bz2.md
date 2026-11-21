---
title: std.bz2
description: bzip2 files read as Python's bz2.decompress reads them, every stream and the same errors, and compressed byte for byte as bz2.compress writes them.
---

# `std.bz2`

`std.bz2` decompresses bzip2 files, such as release archives in `.tar.bz2` and what `bzip2`, `pbzip2` and Python's `bz2` write. It gives the same bytes as Python 3.14's `bz2.decompress`, with libbzip2 1.0.8, reading every stream in a file one after another, and on broken files it fails where Python fails. It also compresses, into the same bytes as Python's `bz2.compress` at each level: see [Compressing](#compressing). [`std.tar`](tar.md) opens and writes a `.tar.bz2` with it.

```calx
import std.bytes.{from_hex}
import std.bz2.{Error, compress, decompress, is_bz2}

fn main() {
    // What Python's `bz2.compress(b"hello, bz2\n")` gives.
    let packed = from_hex("425a683931415926535976f92ca800000359800010400410001244801020003100302069a3200995a9c3f17724538509076f92ca80")!
    println("{is_bz2(packed)}")    // out: true
    print(String.from_utf8(decompress(packed.copy())!) ?? "bad")    // out: hello, bz2
    println("{compress("hello, bz2\n".to_bytes()) == packed}")    // out: true

    // A limit on what comes out, for bytes that came from elsewhere.
    match decompress(packed, 5) {
        ok(_) => println("fits"),
        err(Error.TooLong) => println("too long"),    // out: too long
        err(e) => println("{e}"),
    }
    match decompress("not bzip2".to_bytes()) {
        ok(_) => println("?"),
        err(e) => println("{e}"),    // out: not a bzip2 file
    }
}
```

| Item | Gives |
| --- | --- |
| `decompress(bytes: List<u8>, limit: u64 = u64.MAX) -> List<u8> ! Error` | What the bytes decompress to, every stream in them one after another, as `bz2.decompress(bytes)` gives it. Fails with `TooLong` if that's more than `limit` bytes, which is no limit unless it's given |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, limit: u64, out: mut List<u8>) ! Error` | Decompresses `bytes[from..to]` onto the end of `out`, at most `limit` bytes. On an error, `out` is as it was. Panics unless the range is in `bytes` |
| `compress(bytes: List<u8>, level: u32 = 9) -> List<u8>` | The bytes compressed into one stream, the same bytes as `bz2.compress(bytes, level)` and `bzip2 -1` to `-9` write. Panics for a level outside 1 to 9, where Python raises a `ValueError`. See [Compressing](#compressing) |
| `compress_into(bytes: List<u8>, from: usize, to: usize, level: u32, out: mut List<u8>)` | Compresses `bytes[from..to]` onto the end of `out`, as `compress` does. Panics unless the range is in `bytes` |
| `is_bz2(bytes: List<u8>) -> bool` | Whether the bytes start as a bzip2 stream does: `BZh` and a digit from 1 to 9, the size of its blocks in 100 kB |

Why bytes don't decompress, as `Error`:

| Error | When | `{e}` | Python raises |
| --- | --- | --- | --- |
| `Format` | They don't start with `BZh` and a digit from 1 to 9 | not a bzip2 file | `OSError`: Invalid data stream |
| `Corrupt` | A block's marker, its tables, its symbols or where its text starts are wrong | corrupt data | `OSError`: Invalid data stream |
| `Checksum` | A block's CRC, or the stream's, doesn't match what it decompressed to | checksum mismatch | `OSError`: Invalid data stream |
| `Truncated` | They end inside a stream | the data ends early | `ValueError`: Compressed data ended before the end-of-stream marker was reached |
| `TooLong` | What they decompress to is more than the limit | longer than the limit when decompressed | |

- Streams follow one another to the end of the bytes, as `pbzip2` writes them and `cat a.bz2 b.bz2` joins them, and each is checked by its CRC as it ends.
- After the first stream, as in Python, bytes that aren't a stream, or a stream that's broken, are left alone: what came before them is the result, without anything the broken stream gave. So zeros or other padding after a file read as nothing. A stream that starts but is cut short is still `Truncated`, even after the first, and so is a lone `B` or `BZ`.
- Nothing at all decompresses to nothing, as in Python, and so does a stream with no blocks, which is what compressing nothing makes.
- Blocks marked randomised, which `bzip2` before 0.9.5 wrote when sorting a block took too long, are undone as libbzip2 undoes them.
- The limit counts what comes out, so a few bytes that would decompress to gigabytes fail without the memory: a megabyte of zeros is 48 bytes. Python has no limit for `decompress`; `std.bz2` has none by default either.
- `Format`, `Corrupt` and `Checksum` are what Python reports as one `OSError` with the message "Invalid data stream": libbzip2 says `BZ_DATA_ERROR_MAGIC` for the first and `BZ_DATA_ERROR` for the others.

## Compressing

`compress` writes the bytes Python's `bz2.compress` writes, which are libbzip2's, at each level. The level is the size of a block in 100 kB, after runs of 4 to 255 of a byte are written as 4 and a count: 9, the default, is the smallest, and 1 needs the least memory to decompress, 400 kB rather than 3.6 MB.

A block's rotations are sorted for the Burrows-Wheeler transform, then move-to-front coded, and its symbols coded with 2 to 6 Huffman tables, each picked for 50 symbols at a time, as libbzip2 picks and remakes them in four rounds, with codes of at most 17 bits. Only the rotations of a block that's one piece repeated can come out in more than one order, and those come out as libbzip2 orders them, as its sort for such blocks is the one `std.bz2` uses for all of them.

Where each block ends depends only on what comes before it, so the input is cut into blocks first, and up to 8 blocks are sorted and coded at once, on the threads of [`parallel for`](../reference/parallel.md). The stream is the same however many threads made it. Each block being compressed takes about 11 bytes for each of its own, 10 MB at level 9.

## Speed

On an x64 machine with 32 threads, in release builds, against Python 3.14's `bz2` with libbzip2 1.0.8, the best of five runs at level 9:

| Input | Size | Compressed | `compress` | Python | `decompress` | Python |
| --- | --- | --- | --- | --- | --- | --- |
| The repository's std, runtime and docs, as a tar | 17.1 MB | 3.70 MB | 0.35 s | 0.69 s | 0.22 s | 0.27 s |
| Text from 12 words and numbers | 2.5 MB | 331 KB | 0.083 s | 0.118 s | 0.044 s | 0.039 s |
| A program | 3.0 MB | 838 KB | 0.090 s | 0.129 s | 0.043 s | 0.054 s |
| Random bytes | 1.3 MB | 1.31 MB | 0.110 s | 0.075 s | 0.033 s | 0.044 s |

A single block compresses about half as fast as libbzip2's, as libbzip2 sorts most blocks with a faster sort and keeps the one `std.bz2` uses for blocks that are one piece repeated; several blocks at once make up for it. Decompressing is about as fast as libbzip2's, and faster on three of these four, as most symbols are decoded by one look in a table of their first 10 bits, and runs are written with `memset`, with the CRC worked out over what came out afterwards.

## How it's checked

`std.bz2` was run on Python 3.14's `bz2`, with libbzip2 1.0.8, on Windows:

- 22 inputs, from empty to 3 MB: text, random bytes, zeros, runs of every length around 4 and 255, all 256 bytes, two symbols, a program, a block's size within 30 bytes, and pieces of 2, 3, 777 and 5,000 bytes repeated, as libbzip2 orders their rotations. Each was compressed by Python at every level up to 200 KB and at levels 1, 5 and 9 above: 138 streams, 48 MB in all. `compress` gave the same bytes for every one, and `decompress` read each back.
- 7,653 broken inputs: every cut and every flipped bit of three small files, one of them two streams, 1,500 flipped bits and 200 cuts of a file of two blocks, 21 kinds of bytes after one stream and after two, such as zeros, `B`, `BZh0`, a broken stream and one cut short, and 600 runs of random bytes, half of them after `BZh9`. Each gave what Python gave: the same bytes, or `Truncated` where Python raised a `ValueError` and another error where it raised an `OSError`.
- A file with a randomised block, made by an encoder that randomises each block as bzip2 0.9.0 did, which Python reads.

The [tests](../../tests/std/bz2.calx) hold Python's bytes and Python's outcomes, and [those of compress](../../tests/std/bz2_compress.calx) the hashes of what Python compressed, from the same generators.

## How it's built

Decompressing is [runtime/calx_bz2.c](../../runtime/calx_bz2.c), linked only into programs that call it, and compressing [runtime/calx_bz2enc.c](../../runtime/calx_bz2enc.c), linked only into programs that compress. The streams are read in Calx over them: [std/bz2.calx](../../std/bz2.calx). The decoder makes libbzip2's checks in libbzip2's order, so a broken file fails where libbzip2's does, and has its table of numbers for randomised blocks. The encoder's sort, its Huffman code lengths and its choice of tables are adapted from libbzip2 1.0.8, so its bytes are the same. Both have libbzip2's notice.

| Part | How |
| --- | --- |
| Bits | Read 32 at a time into a 64-bit buffer, most significant first |
| Huffman | A table of the first 10 bits for each of a block's tables, built by decoding each 10 bits as libbzip2 would; a longer code, or one that's wrong, is decoded a bit at a time as libbzip2 decodes it |
| The transform | Undone as libbzip2 undoes it, with a vector of 4 bytes for each byte of the block, which a block's text follows from where it starts |
| Output | A run is written with `memset`, and the CRC worked out over what came out, a table a byte. When the room runs out mid-block, the decoder stops, and carries on where it was once there's more |
| Sorting rotations | libbzip2's fallback sort: by the first byte, then by the first 2, 4, 8 and so on, each round quicksorting the groups that are still tied |
| All the processors | Up to 8 blocks at once, each into bits of its own, put one after another in order |
