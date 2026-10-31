---
title: std.lz4
description: LZ4 block streams as lz4-java's LZ4BlockOutputStream writes them and LZ4BlockInputStream reads them - what Minecraft's region files hold when region-file-compression is lz4.
---

# `std.lz4`

When server.properties says `region-file-compression=lz4`, Minecraft stores each chunk in a region file with lz4-java's `LZ4BlockOutputStream`, and reads it back with `LZ4BlockInputStream`. Minecraft ships lz4-java 1.10.1. LZ4 is much faster than zlib, and the files come out bigger.

`std.lz4` writes the same stream format, and Java reads it. It reads what Java writes, and it accepts and refuses the same broken streams as `LZ4BlockInputStream` does.

```calx
import std.lz4.{compress, decompress, decompress_into}

fn main() {
    let text = "hello hello hello hello hello hello".to_bytes()
    let packed = compress(text)
    println("{text.len()} -> {packed.len()}")    // out: 35 -> 58

    // A limit on what comes out, so a few bytes in a region file can't fill the memory.
    let back = decompress(packed.copy(), 1000)!
    println(String.from_utf8(back) ?? "bad")    // out: hello hello hello hello hello hello

    // Onto the end of a list, stopping at the limit.
    var out = "> ".to_bytes()
    let ended = decompress_into(packed, 0, packed.len(), 5, mut out)!
    println("{ended} {String.from_utf8(out) ?? "bad"}")    // out: false > hello
}
```

| Item | Gives |
| --- | --- |
| `compress(bytes: List<u8>) -> List<u8>` | The bytes compressed in blocks of 64 KiB, as `new LZ4BlockOutputStream(out)` writes them. A block that doesn't get smaller is stored as it is |
| `compress_into(bytes: List<u8>, from: usize, to: usize, out: mut List<u8>)` | Compresses `bytes[from..to]` onto the end of `out`. Panics unless the range is in `bytes` |
| `decompress(bytes: List<u8>, limit: usize) -> List<u8> ! Error` | What the stream decompresses to. Fails with `TooLong` if that's more than `limit` bytes |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, limit: usize, out: mut List<u8>) -> bool ! Error` | Decompresses `bytes[from..to]` onto the end of `out`, at most `limit` bytes. `true` if the stream ended, `false` if it stopped at the limit with more to come. On an error, `out` keeps the blocks before it |
| `xxh32(bytes: List<u8>, seed: u32) -> u32` | The XXH32 hash, as lz4-java's `XXHash32.hash` gives it. Each block's checksum is its low 28 bits, with the seed `0x9747B28C` |

Why bytes don't decompress, as `Error`:

| Error | When |
| --- | --- |
| `Corrupt` | They aren't a stream `LZ4BlockInputStream` accepts: a bad header, or a block its decompressor refuses |
| `Truncated` | They end before the stream does |
| `Checksum` | A block's checksum doesn't match what it decompresses to |
| `TooLong` | The data is more than `decompress`'s limit |

Java throws an `IOException` for all of the first three, "Stream is corrupted" or "Stream ended prematurely". `std.lz4` says which.

- The stream is a run of blocks, each with a 21-byte header: `LZ4Block`, a byte for the method and the block size, then the compressed length, the length and the checksum, little-endian. An empty block ends it. Bytes after that are ignored, as Java ignores them.
- `compress` writes blocks of 64 KiB, Java's default. Streams with blocks of up to 32 MiB, which Java allows, decompress too.
- lz4-java's decompressor isn't the reference one, and it takes some blocks the LZ4 format says no compressor writes: a last match that starts closer than 12 bytes to the end, and a match 0 bytes back, which it fills with zeros. It decides that literals end a block by how many bytes are left in its own buffer, not in the block. That buffer is 21 bytes at first and grows with the blocks, so the same block can be refused at the start of a stream and taken after a longer one. `std.lz4` follows each of those rules.
- The bytes aren't always Java's bytes. `std.lz4` picks its own matches, so its output is its own, though Java reads it. For the short inputs in the tests, it's the same.

## Checked against Java

- The corpus `std.zlib` was checked with: 2414 records, 49.8 MB. lz4-java 1.10.1 on Java 25 decompressed what `std.lz4` compressed, and `std.lz4` decompressed what Java compressed. Every record came back.
- 500,000 broken streams: Java's output with bytes flipped, cut, added or made up, and single blocks made from changed LZ4 data or written sequence by sequence near the rules about a block's end, half of them after a whole block. `std.lz4` gave the same outcome as `LZ4BlockInputStream` for every one: the same data, or an error where Java threw.
- The [tests](../../tests/std/lz4.calx) hold Java's bytes and Java's outcomes.

## How it's built

[runtime/calx_lz4.c](../../runtime/calx_lz4.c), linked only into programs that call it, written from the LZ4 block format's description and black-box tests of lz4-java.

| Part | How |
| --- | --- |
| Compressing | Greedy matching through a table of 8192 hashes of 4 bytes, skipping ahead faster the longer nothing matches |
| Decompressing | A fast loop while 32 bytes of input and 64 of room are left, which copies 16 bytes of literals at once and short matches in two or three fixed copies. A careful loop for the rest, with lz4-java's checks |
| Copying a match | 16 bytes at a time when the distance is 16 or more, and 8 bytes at a time from a whole number of periods back when it's shorter |
| XXH32 | Four lanes, 16 bytes a step |
| The output | `decompress` reads the lengths in the headers first, so most streams decompress in one call without copying |

On an i9-13900K, one run each over the corpus:

| | Calx | Java 25, lz4-java 1.10.1 |
| --- | --- | --- |
| Compressing | 1490 MB/s | 601 MB/s |
| Decompressing | 2806 MB/s | 1051 MB/s |
| Output | 12,033,205 bytes | 12,033,271 bytes |

`std.lz4` is written in Calx over those runtime calls: [std/lz4.calx](../../std/lz4.calx).
