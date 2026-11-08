---
title: std.xz
description: .xz, .lzma and .lz files decompressed as Python's lzma.decompress reads them, with every filter and check, and xz -T's blocks decoded at once on all the processors.
---

# `std.xz`

`std.xz` decompresses .xz files, such as release archives in `.tar.xz`, and the older .lzma files and lzip's .lz files. It reads what `xz` and Python's `lzma` write, and it decompresses the same bytes as Python's `lzma.decompress` does. It fails on the same broken files, for the same reasons. [`std.tar`](tar.md) opens a `.tar.xz` with it.

It doesn't compress yet.

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

| Item | Gives |
| --- | --- |
| `decompress(bytes: List<u8>, format: Format = Format.Auto, limit: u64 = u64.MAX) -> List<u8> ! Error` | What the bytes decompress to, as `lzma.decompress(bytes)` gives it. Fails with `TooLong` if that's more than `limit` bytes, which is no limit unless it's given |
| `decompress_into(bytes: List<u8>, from: usize, to: usize, format: Format, limit: u64, out: mut List<u8>) ! Error` | Decompresses `bytes[from..to]` onto the end of `out`, at most `limit` bytes. On an error, `out` is as it was. Panics unless the range is in `bytes` |
| `is_xz(bytes: List<u8>) -> bool` | Whether the bytes start as an .xz file does: `fd 37 7a 58 5a 00` |
| `crc64(bytes: List<u8>) -> u64` | The CRC-64 .xz files check blocks with: ECMA-182's, reflected, as `lzma_crc64` gives it |

Which files `decompress` takes, as `Format`:

| Format | Takes | Python's |
| --- | --- | --- |
| `Auto` | .xz, .lzma and .lz files, told apart by how they start | `FORMAT_AUTO` |
| `Xz` | .xz files only | `FORMAT_XZ` |
| `Alone` | .lzma files only, with any dictionary size in the header | `FORMAT_ALONE` |

Why bytes don't decompress, as `Error`:

| Error | When | `{e}` |
| --- | --- | --- |
| `Format` | They don't start as a file of the format, or of any format for `Auto` | not an .xz or .lzma file |
| `Options` | A filter, a check or a header flag that the format reserves, or settings no encoder writes | a filter or setting that isn't supported |
| `Corrupt` | The headers, the index or the compressed data are wrong | corrupt data |
| `Checksum` | A block's CRC-32, CRC-64 or SHA-256, or an .lz member's CRC-32, doesn't match what it decompressed to | checksum mismatch |
| `Truncated` | They end before the file does | the data ends early |
| `TooLong` | What they decompress to is more than the limit | longer than the limit when decompressed |

Python raises an `LZMAError` for the first four, with "Input format not supported by decoder", "Invalid or unsupported options" or "Corrupt input data", and an `EOFError` for `Truncated`. A wrong check is "Corrupt input data" in Python. `std.xz` says which it is.

- Files one after another decompress one after another, as `xz` writes them with `xz -c a b`, and they don't have to be of one format. After the first file, `decompress` stops quietly at bytes that aren't the start of another, as Python does. It fails if they are the start of one, cut short. Null bytes after an .xz stream, padding in fours, are the start of one.
- Every filter .xz has: LZMA2 last, and up to three of delta, x86, PowerPC, IA-64, ARM, ARM-Thumb, ARM64, SPARC and RISC-V before it, with their start offsets. Every check: none, CRC-32, CRC-64 and SHA-256. Checks of the IDs the format reserves for later aren't checked, as liblzma doesn't check them.
- `Auto` takes only the .lzma headers LZMA's tools write, as liblzma's auto decoder does: dictionary sizes of 2^n or 2^n + 2^(n-1), and sizes under 256 GiB, since so much else would pass for one. `Alone` takes any header.
- An .lz file's members are version 0 or 1, and their CRC-32, size and member size are all checked, as liblzma reads them since xz 5.4.
- The limit counts what comes out, so a few bytes that would decompress to gigabytes fail without the memory. Python has no limit for `lzma.decompress`; `std.xz` has none by default either.

## Speed

`std.xz` reads the block sizes `xz` writes into each block's header, reserves all the room at once, and writes each block straight into its place. When a file has several blocks with their sizes, as `xz -T` writes them by default, it decodes them on all the processors at once with [`parallel for`](../reference/parallel.md)'s threads.

On an i9-13900K running other programs, best of 7 runs each, so the numbers are rough. `src.tar` is 16.9 MB of source files, and `bin.exe` a 6.2 MB Windows program. The `mt` files are `xz -T0 --block-size=2MiB`'s, with 9 and 3 blocks:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: lzma-rust2 0.25 | Rust: lzma-rs 0.3 |
| --- | --- | --- | --- | --- | --- |
| src.tar.xz | 94 ms | 156 ms | 141 ms | 150 ms | 295 ms |
| src.tar.mt.xz | 17 ms | 154 ms | 29 ms | 30 ms | 304 ms |
| bin.exe.xz | 41 ms | 65 ms | 74 ms | 75 ms | 124 ms |
| bin.exe.mt.xz | 16 ms | 64 ms | 34 ms | 33 ms | 127 ms |

- Python and the liblzma crate run liblzma 5.8, Python on one thread. Each Rust crate's time is from whichever of its decoders, plain or threaded, was faster on the file: the threaded ones on the `mt` files. Each Rust decoder had the room for its output reserved before it started.
- The Rust programs were built with `lto = "fat"`, one codegen unit and `target-cpu=x86-64-v3`, as Calx builds for x86-64-v3.
- liblzma built by GCC, as Linux distributions ship it, is faster than the Windows builds above. In WSL on the same machine, Python with it is level with `std.xz` on the files with one block: 88 ms to 88 on src.tar.xz, and 37 to 38 on bin.exe.xz. On the `mt` files it takes 84 and 36 ms, and `std.xz` 15.

## How it's checked

`std.xz` was run on 67,772 files, and for each it gave what Python 3.14's `lzma.decompress` with liblzma 5.8.3 gave: the same bytes, or the same kind of error, on Windows and on Linux.

- Text, programs, random bytes and zeros, from empty to 5 MB, compressed by Python at every preset, with every check, filter and chain of filters, and LZMA settings from `lc=0` to `pb=4`. `xz`'s own files: `-T4` blocks, block lists, ARM64 and RISC-V, `--format=lzma`. .lzma and .lz files, lzip versions 0 and 1.
- The other formats' files given to `Xz` and `Alone`.
- Each of those followed by another of another format, by padding of seven lengths up to 30 bytes, by a file cut in half, and by garbage.
- Every cut of nine small files, at every byte.
- 65,400 files with bytes flipped, changed, added, removed, swapped and added at the end, 25,000 of them from `xz -T`, with blocks that decode at once, and 400 from files of up to 800 KB.

The [tests](../../tests/std/xz.calx) hold Python's bytes and Python's outcomes.

## How it's built

[runtime/calx_xz.c](../../runtime/calx_xz.c), linked only into programs that call it, written from the .xz file format's specification and the LZMA algorithm as the public-domain LZMA SDK describes it.

| Part | How |
| --- | --- |
| The range decoder | Kept in registers, with a fast loop while 64 bytes of input are left that doesn't check where the input ends, and a careful one for the rest |
| Bits in trees | Without branches, and with the next level's probabilities loaded before the bit is known |
| Literals after a match | Both of the next probabilities loaded at once |
| Copying a match | 16 bytes at a time when the distance is at least the length, never past its end, so blocks next to each other can decode at the same time |
| CRC-64 | Folded with the processor's carry-less multiply in four lanes, 64 bytes a step, at about 39 GB/s on bytes in the cache, with tables where it's missing |
| CRC-32 and SHA-256 | `std.zlib`'s and `std.hash`'s |

`std.xz` reads the headers, the index and the footer in Calx over those runtime calls, in the order liblzma checks them, so broken files fail the same way: [std/xz.calx](../../std/xz.calx).
