---
title: std.zlib
description: Compressing and inflating zlib, gzip and raw deflate streams, which read and write what Java's Deflater, Inflater and GZIP streams do.
---

# `std.zlib`

Minecraft's server compresses three things, all at level 6, Java's default:

- Each packet longer than the server's compression threshold, with zlib.
- Each chunk in a region file, with zlib.
- NBT files such as `level.dat`, with gzip.

`std.zlib` reads what Java's `Deflater` and `GZIPOutputStream` write, and writes what its `Inflater` and `GZIPInputStream` read. It accepts and refuses the same broken streams as they do.

```calx
import std.zlib.{Format, compress, compress_into, crc32, decompress, decompress_into, decompress_stream}

fn main() {
    let text = "hello hello hello hello".to_bytes()
    let packed = compress(text, Format.Zlib, 6)
    println("{text.len()} -> {packed.len()}")    // out: 23 -> 15

    // A limit on what comes out, so a few bytes from a client can't fill the memory.
    let back = decompress(packed.copy(), Format.Zlib, 1000)!
    println(String.from_utf8(back) ?? "bad")    // out: hello hello hello hello

    // gzip, as in NBT files, with its CRC-32 checked.
    let nbt = compress("level.dat".to_bytes(), Format.Gzip, 6)
    println("{nbt.len()} {crc32("level.dat".to_bytes())}")    // out: 29 1806400839

    // Onto the end of a list, stopping at the limit.
    var out = "> ".to_bytes()
    let ended = decompress_into(packed, 0, packed.len(), Format.Zlib, 5, mut out)!
    println("{ended} {String.from_utf8(out) ?? "bad"}")    // out: false > hello

    // Two streams back to back, as zip entries sit: the first's end is where the second starts.
    var two: List<u8> = []
    compress_into("one".to_bytes(), 0, 3, Format.Raw, 6, mut two)
    compress_into("two".to_bytes(), 0, 3, Format.Raw, 6, mut two)
    var first: List<u8> = []
    let end = decompress_stream(two, 0, two.len(), Format.Raw, 100, mut first)!
    var second: List<u8> = []
    _ = decompress_stream(two, end, two.len(), Format.Raw, 100, mut second)!
    println("{String.from_utf8(first) ?? "bad"} {String.from_utf8(second) ?? "bad"}")    // out: one two
}
```

| Item | Gives |
| --- | --- |
| `Format` | `Zlib` (RFC 1950, Java's `Deflater` and `Inflater`), `Gzip` (RFC 1952, Java's GZIP streams) or `Raw` (deflate alone, Java's `nowrap`) |
| `compress(bytes: List<u8>, format: Format, level: u32) -> List<u8>` | The bytes compressed at `level`. 0 stores them, 1 is the fastest, 9 the smallest. Panics for a level past 9 |
| `compress_into(bytes: List<u8>, from: usize, to: usize, format: Format, level: u32, out: mut List<u8>)` | Compresses `bytes[from..to]` onto the end of `out`, as a packet goes after its length. Panics unless the range is in `bytes` and under 4 GiB |
| `decompress(bytes: List<u8>, format: Format, limit: usize) -> List<u8> ! Error` | What the stream inflates to. Fails with `TooLong` if that's more than `limit` bytes |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, format: Format, limit: usize, out: mut List<u8>) -> bool ! Error` | Inflates `bytes[from..to]` onto the end of `out`, at most `limit` bytes. `true` if the stream ended, `false` if it stopped at the limit with more to come, as `Inflater.inflate` stops when its array is full |
| `decompress_stream(bytes: List<u8>, from: usize, to: usize, format: Format, limit: usize, out: mut List<u8>) -> usize ! Error` | Inflates the one stream that starts at `from` onto the end of `out`, and gives the index just past its last byte, where whatever follows it starts: a zip entry's data descriptor, or another stream. Fails with `TooLong` past `limit` bytes |
| `crc32(bytes: List<u8>) -> u32` | The CRC-32, as Java's `CRC32` gives it |
| `crc32_range(bytes: List<u8>, from: usize, to: usize, crc: u32 = 0) -> u32` | The CRC-32 of the bytes from `from` up to `to`, carried on from `crc`, as Java's `CRC32.update(bytes, off, len)` carries it. Panics unless the range is in `bytes` |
| `adler32(bytes: List<u8>) -> u32` | The Adler-32, as Java's `Adler32` gives it |

Why bytes don't inflate, as `Error`:

| Error | When | `{e}` |
| --- | --- | --- |
| `Corrupt` | They aren't a stream zlib accepts: a bad header, a code that can't be, or a distance back past the start | corrupt data |
| `Truncated` | They end before the stream does | the data ends early |
| `Checksum` | The Adler-32, the CRC-32, gzip's length or gzip's header CRC doesn't match | checksum mismatch |
| `TooLong` | The data is more than `decompress`'s limit | longer than the limit when inflated |
| `Dictionary` | The zlib header asks for a preset dictionary. Java's `Inflater` stops there without an error, and `needsDictionary()` says why. Neither Minecraft nor `std.zlib` uses one | the data needs a preset dictionary |

- Bytes after the end of the stream are ignored, as Java ignores them.
- gzip reads on into the next member when one follows, as `GZIPInputStream` does, and ignores what follows if it isn't a whole gzip header.
- Minecraft's packet decoder gives `Inflater` an array of the length the packet says, then checks that it was filled. `decompress_into` with that length as the limit does the same: if it gives `false`, or adds fewer bytes, the packet is bad.
- The bytes aren't always Java's bytes. Deflate allows many ways to write the same data, and `std.zlib` picks its own matches, so its output is its own, though Java reads it. On Minecraft's region files it comes out smaller than Java's at every level, and at 6, vanilla's, 99.8% of Java's bytes ([below](#how-its-built)).

## Checked against Java

- A corpus of 2414 records, 49.8 MB: Minecraft's structure files unpacked, 300 of its source files, random bytes, runs and repeats from 0 bytes to 1 MiB. Java 25's `Inflater` and `GZIPInputStream` inflated what `std.zlib` wrote, and `std.zlib` inflated what Java wrote, for levels 0, 1, 6 and 9 and all three formats. Every record came back.
- 240,000 broken streams: Java's output with bytes flipped, cut, added or made up, and blocks of random codes. `std.zlib` gave the same outcome as `Inflater` for every one: the same data, or an error where Java threw, or the same stop where Java wanted a dictionary.
- 25 gzip cases, with header fields, FHCRC, reserved flags, a second member and bytes after the end, each gave what `GZIPInputStream` gave.
- The [tests](../../tests/std/zlib.calx) hold Java's bytes and check the rest by round trips.

## How it's built

[runtime/calx_zlib.c](../../runtime/calx_zlib.c), linked only into programs that call it, written from RFC 1950, 1951 and 1952.

| Part | How |
| --- | --- |
| Inflating | Lookup tables of 10 bits for literals and lengths, and 8 for distances, with second tables under them for longer codes. A fast loop takes 64 bits of input at a time, and up to three literals for each read, while there are 16 bytes of input and 274 of room left. A slow loop takes the last bytes one code at a time |
| Copying a match | 16 bytes at a time when the distance is 16 or more. For shorter distances, the pattern is built once and then copied 8 bytes at a time |
| Compressing | Hash chains on 4 bytes, searched further at each level. From level 4, as libdeflate does: matches of 3 bytes too, within 8 KiB, from a table of their own; before a match is taken, the next position searched half as hard, and its match taken if it's longer by more than its distance costs; and a block ended where the kinds of its symbols change. From level 4 the match at the last match's distance is tried too, which finds an image's row above when the chains are too full of commoner strings to reach it. Each block is written stored, with the fixed codes or with its own codes, whichever is smallest |
| Adler-32 | 32 bytes a step with AVX2 where the processor has it, and with NEON on Arm64. Otherwise a byte at a time, 8 to a loop |
| CRC-32 | From 64 bytes on, folded 64 bytes at a time with carry-less multiplies: PCLMUL on x86, PMULL on Arm64. Then the last bytes, and shorter inputs, with Arm64's CRC32 instructions, 8 bytes each, or otherwise 8 tables, 8 bytes a step |
| The output | `decompress` guesses the length from how the first bytes inflated, and for gzip, from the length in its trailer, so most streams inflate in one call without copying |

On an i9-13900K, one run each over the corpus (level 1 and inflating; the lazy levels have been rebuilt since, and are measured below):

| | Calx | Java 25 |
| --- | --- | --- |
| Compressing, level 1 | 578 MB/s | 288 MB/s |
| Inflating zlib | 1937 MB/s | 1022 MB/s |
| Inflating gzip | 1090 MB/s | 671 MB/s |
| A packet-sized record inflated | 2.1 µs | 4.1 µs |

At level 1 the output is 1.3% smaller than Java's.

The checksums, from [bench/digest](../../bench/README.md#hashes-checksums-and-aes) on the i9-13900K's performance cores, over 64 MiB, the best of 5, against Python 3.14.6's `zlib`, which is zlib-ng's:

| | Calx | Python |
| --- | --- | --- |
| CRC-32 | 14,500 MiB/s | 12,300 MiB/s |
| Adler-32 | 17,000 MiB/s | 16,100 MiB/s |

The tables alone did CRC-32 at 2,600 MiB/s, before it was folded with PCLMUL (CLX-281).

Minecraft's region files, compressed again: 2,000 of the chunks vanilla 26.3 saved, 68.9 MB inflated, each deflated as a zlib stream on one P-core, best of five runs for Calx and of up to three for Java 25's `Deflater`. Java's level 6 gives back vanilla's own bytes exactly.

| Level | Calx, µs a chunk | Java, µs a chunk | Calx's bytes, of Java's |
| --- | --- | --- | --- |
| 1 | 74 | 134 | 94.6% |
| 4 | 127 | 221 | 98.9% |
| 5 | 135 | 298 | 99.3% |
| 6 | 158 | 446 | 99.8% |
| 7 | 176 | 571 | 99.9% |
| 8 | 261 | 1416 | 99.8% |
| 9 | 339 | 3258 | 99.5% |

Since then, levels 4 to 9 also try the last match's distance. On 2,000 of Calcite's Terralith chunks, that took about 2.5% longer at level 6, and the bytes changed by less than 0.02%. On a dithered palette image from [std.image](image.md), level 6 came out a third smaller: from 150% of zlib's bytes to 102%.

`std.zlib` is written in Calx over those runtime calls: [std/zlib.calx](../../std/zlib.calx).
