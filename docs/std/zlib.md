---
title: std.zlib
description: zlib, gzip and raw deflate, compressing and inflating as Java's Deflater, Inflater and GZIP streams do - what vanilla's packets, region files and NBT files use.
---

# `std.zlib`

Vanilla compresses three things, all at level 6, Java's default:

- Each packet longer than the server's compression threshold, with zlib.
- Each chunk in a region file, with zlib.
- NBT files such as `level.dat`, with gzip.

`std.zlib` reads what Java's `Deflater` and `GZIPOutputStream` write, and writes what its `Inflater` and `GZIPInputStream` read. It accepts and refuses the same broken streams as they do.

```calx
import std.zlib.{Format, compress, crc32, decompress, decompress_into}

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
}
```

| Item | Gives |
| --- | --- |
| `Format` | `Zlib` (RFC 1950, Java's `Deflater` and `Inflater`), `Gzip` (RFC 1952, Java's GZIP streams) or `Raw` (deflate alone, Java's `nowrap`) |
| `compress(bytes: List<u8>, format: Format, level: u32) -> List<u8>` | The bytes compressed at `level`. 0 stores them, 1 is the fastest, 9 the smallest. Panics for a level past 9 |
| `compress_into(bytes: List<u8>, from: usize, to: usize, format: Format, level: u32, out: mut List<u8>)` | Compresses `bytes[from..to]` onto the end of `out`, as a packet goes after its length. Panics unless the range is in `bytes` and under 4 GiB |
| `decompress(bytes: List<u8>, format: Format, limit: usize) -> List<u8> ! Error` | What the stream inflates to. Fails with `TooLong` if that's more than `limit` bytes |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, format: Format, limit: usize, out: mut List<u8>) -> bool ! Error` | Inflates `bytes[from..to]` onto the end of `out`, at most `limit` bytes. `true` if the stream ended, `false` if it stopped at the limit with more to come, as `Inflater.inflate` stops when its array is full |
| `crc32(bytes: List<u8>) -> u32` | The CRC-32, as Java's `CRC32` gives it |
| `adler32(bytes: List<u8>) -> u32` | The Adler-32, as Java's `Adler32` gives it |

Why bytes don't inflate, as `Error`:

| Error | When |
| --- | --- |
| `Corrupt` | They aren't a stream zlib accepts: a bad header, a code that can't be, or a distance back past the start |
| `Truncated` | They end before the stream does |
| `Checksum` | The Adler-32, the CRC-32, gzip's length or gzip's header CRC doesn't match |
| `TooLong` | The data is more than `decompress`'s limit |
| `Dictionary` | The zlib header asks for a preset dictionary. Java's `Inflater` stops there without an error, and `needsDictionary()` says why. Neither vanilla nor `std.zlib` uses one |

- Bytes after the end of the stream are ignored, as Java ignores them.
- gzip reads on into the next member when one follows, as `GZIPInputStream` does, and ignores what follows if it isn't a whole gzip header.
- Vanilla's packet decoder gives `Inflater` an array of the length the packet says, then checks that it was filled. `decompress_into` with that length as the limit does the same: if it gives `false`, or adds fewer bytes, the packet is bad.
- The bytes aren't always Java's bytes. Deflate allows many ways to write the same data, and `std.zlib` picks its own matches, so its output is its own, though Java reads it. At levels 1 to 6 it comes out about 1% smaller than Java's.

## Checked against Java

- A corpus of 2414 records, 49.8 MB: vanilla's structure files unpacked, 300 of its source files, random bytes, runs and repeats from 0 bytes to 1 MiB. Java 25's `Inflater` and `GZIPInputStream` inflated what `std.zlib` wrote, and `std.zlib` inflated what Java wrote, for levels 0, 1, 6 and 9 and all three formats. Every record came back.
- 240,000 broken streams: Java's output with bytes flipped, cut, added or made up, and blocks of random codes. `std.zlib` gave the same outcome as `Inflater` for every one: the same data, or an error where Java threw, or the same stop where Java wanted a dictionary.
- 25 gzip cases, with header fields, FHCRC, reserved flags, a second member and bytes after the end, each gave what `GZIPInputStream` gave.
- The [tests](../../tests/std/zlib.calx) hold Java's bytes and check the rest by round trips.

## How it's built

[runtime/calx_zlib.c](../../runtime/calx_zlib.c), linked only into programs that call it, written from RFC 1950, 1951 and 1952.

| Part | How |
| --- | --- |
| Inflating | Lookup tables of 10 bits for literals and lengths, and 8 for distances, with second tables under them for longer codes. A fast loop takes 64 bits of input at a time, and up to three literals for each read, while there are 16 bytes of input and 274 of room left. A slow loop takes the last bytes one code at a time |
| Copying a match | 16 bytes at a time when the distance is 16 or more. For shorter distances, the pattern is built once and then copied 8 bytes at a time |
| Compressing | Hash chains on 4 bytes, searched further at each level, with lazy matching from level 4. Each block is written stored, with the fixed codes or with its own codes, whichever is smallest |
| Adler-32 | AVX2 where the processor has it, 32 bytes a step. Otherwise a byte at a time, 8 to a loop |
| CRC-32 | 8 tables, 8 bytes a step |
| The output | `decompress` guesses the length from how the first bytes inflated, and for gzip, from the length in its trailer, so most streams inflate in one call without copying |

On an i9-13900K, one run each over the corpus:

| | Calx | Java 25 |
| --- | --- | --- |
| Compressing, level 6 | 158 MB/s | 89 MB/s |
| Compressing, level 1 | 578 MB/s | 288 MB/s |
| Compressing, level 9 | 28 MB/s | 9.3 MB/s |
| Inflating zlib | 1937 MB/s | 1022 MB/s |
| Inflating gzip | 1090 MB/s | 671 MB/s |
| A packet-sized record compressed, level 6 | 11.4 µs | 19.9 µs |
| A packet-sized record inflated | 2.1 µs | 4.1 µs |

At level 6 the output is 1.1% smaller than Java's, and at level 1, 1.3% smaller. At level 9 it's 2.3% bigger, since it doesn't look for 3-byte matches.

`std.zlib` is written in Calx over those runtime calls: [std/zlib.calx](../../std/zlib.calx).
