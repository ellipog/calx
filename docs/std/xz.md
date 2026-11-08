---
title: std.xz
description: .xz, .lzma and .lz files decompressed as Python's lzma.decompress reads them, with every filter and check, and .xz and .lzma files compressed as xz and lzma.compress write them, both on all the processors.
---

# `std.xz`

`std.xz` decompresses .xz files, such as release archives in `.tar.xz`, and the older .lzma files and lzip's .lz files. It reads what `xz` and Python's `lzma` write, and it decompresses the same bytes as Python's `lzma.decompress` does. It fails on the same broken files, for the same reasons.

It compresses too, at `xz`'s presets, into files that `xz` and Python read. On our benchmark they come out within about one percent of liblzma's size, and at the default preset a file of tens of megabytes is written 5 to 12 times as fast as liblzma writes it, on all the processors. [`std.tar`](tar.md) opens a `.tar.xz` with it, and writes one.

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

Python raises an `LZMAError` for the first four, with "Input format not supported by decoder", "Invalid or unsupported options" or "Corrupt input data", and an `EOFError` for `Truncated`. A wrong check is "Corrupt input data" in Python. `std.xz` says which it is.

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
- **`Format.Alone`** writes a .lzma file with the same settings: the dictionary and the size in the header, then one LZMA stream without an end marker. It has no check, so `check` makes no difference to it. Python writes the size as unknown and ends the stream with a marker, which takes 5 bytes more. Python, `xz` and `std.xz` read both.
- **Slices.** Each block is cut into slices of 1 MiB, which compress at once. Each slice starts with a state reset, which LZMA2 has for this: the coder's probabilities start afresh, but the text before the slice stays in the dictionary, so matches still reach back across the cut. On our benchmark the resets cost about 0.2% of the size.
- **Matches on all the processors.** For presets 4 to 9, finding matches in the binary trees takes two thirds of the time. Each tree holds only the positions whose first four bytes hash to it, and they go in in order, so the trees are shared out among the threads by hash. Each thread walks its own trees over the block and notes the matches it finds. Then the slices are parsed at once from the notes. They're the matches one thread would find. It takes more memory than one thread, as [Speed](#speed) shows. Presets 0 to 3 give each slice hash chains of its own instead, since those take in the dictionary before the slice quickly.
- **The same bytes make the same file**, on any machine with any number of processors. The slices are cut by size, and each finds the same matches however it's run. So a `.tar.xz` built twice is the same file.
- **Short matches**, of two and three bytes, reach back 256 KiB at most, as further back they seldom pay for their distance. That's what lets a slice find the same ones from the text just before it as one thread finds from all of it.
- `compress` panics for a preset past 9, and for `Format.Auto`.

On one thread, `xz` and liblzma write the whole text as one block without slices, as Python does. So what they write is up to about one percent smaller than what `std.xz` writes, and it decompresses on one thread only.

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

`compress` was timed on the same machine, with the same programs running, best of 3 runs, or of 2 for the 40 MB file. `std.txt` is 1.2 MB of Calx source, `src.tar` 18.2 MB of the Calx repository's files, and `clang40` the first 40 MB of `clang.exe`. At preset 6, the default:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: liblzma, threads | Rust: lzma-rust2 0.25 | Rust: lzma-rust2, threads |
| --- | --- | --- | --- | --- | --- | --- |
| std.txt | 112 ms | 389 ms | 379 ms | 421 ms | 450 ms | 522 ms |
| src.tar | 883 ms | 9,967 ms | 10,914 ms | 11,813 ms | 11,490 ms | 8,726 ms |
| clang40 | 2,291 ms | 29,705 ms | 25,868 ms | 12,884 ms | 29,320 ms | 20,006 ms |

At preset 0, the fastest:

| File | Calx | Python 3.14 | Rust: liblzma 0.4.9 | Rust: liblzma, threads | Rust: lzma-rust2 0.25 | Rust: lzma-rust2, threads |
| --- | --- | --- | --- | --- | --- | --- |
| std.txt | 23 ms | 36 ms | 45 ms | 41 ms | 43 ms | 37 ms |
| src.tar | 60 ms | 620 ms | 698 ms | 84 ms | 744 ms | 104 ms |
| clang40 | 227 ms | 2,350 ms | 2,512 ms | 237 ms | 3,887 ms | 331 ms |

- Python and the liblzma crate run liblzma 5.8. The threaded encoders had a thread for each of the 32 processors. They give each block a thread, and cut blocks of three times the dictionary, as `xz -T0` does: 24 MiB at preset 6, so `std.txt` and `src.tar` get one thread and `clang40` two, and 1 MiB at preset 0. `std.xz` cuts the same blocks, and puts all the processors on each.
- At preset 6, what `std.xz` writes is 0.4%, 0.6% and 1.1% bigger than what liblzma writes on one thread, for the three files, and 0.4%, 0.6% and 0.3% bigger than what it writes on threads. At preset 0 it's 3.2%, 3.8% and 1.2% smaller than liblzma's, 0.9% bigger than lzma-rust2's on one thread, and the same size as lzma-rust2's on threads.
- At preset 0 the blocks are 1 MiB, so the threaded encoders have blocks for every processor too, and come near `std.xz`'s time. At preset 6, `std.xz` is 3.4 times as fast as the fastest of the others on `std.txt`, 9.9 times on `src.tar` and 5.6 times on `clang40`.
- `xz` 5.8.3, in WSL on the same machine, took about as long as liblzma: 10.6 s for `src.tar` at preset 6, 9.9 s with `-T0`, and 18.9 s for `clang40` with `-T0`.
- The Rust programs were built as above, and once more without `target-cpu`, and each Rust time is the faster of the two builds' best. Each program checked that what it wrote decompressed to the file.

Finding the matches on all the processors takes more memory than one thread does. The trees have room for the dictionary and a window of text more, and the threads note a window's matches, packed into 4 bytes each, or 5 for presets 7 to 9 with `extreme`, before the slices are parsed from them. Peak memory, as `/usr/bin/time` gives it in WSL on the same machine:

| File | Preset | Calx | Python 3.14 | `xz -T1` | `xz -T0` |
| --- | --- | --- | --- | --- | --- |
| src.tar | 0 | 70 MiB | 39 MiB | 5 MiB | 67 MiB |
| src.tar | 6 | 434 MiB | 127 MiB | 95 MiB | 116 MiB |
| src.tar | 9 | 582 MiB | 254 MiB | 223 MiB | 243 MiB |
| clang40 | 0 | 169 MiB | 82 MiB | 5 MiB | 124 MiB |
| clang40 | 6 | 405 MiB | 166 MiB | 95 MiB | 239 MiB |
| clang40 | 9 | 844 MiB | 480 MiB | 410 MiB | 460 MiB |

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

What `compress` writes was checked on 16 files: text, programs, random bytes, zeros, runs and repeats, from empty to 5 MB. Each was compressed at every preset, at presets 0, 3 and 6 with `extreme`, in blocks of 300 KB and of the preset's size, with every check, and as .lzma: 448 files. `std.xz`, Python 3.14's `lzma` and `xz` 5.8.3 each decompressed all 448 to what went in. Each file also came out the same on 1, 2, 5 and 32 workers. The [tests](../../tests/std/xz_compress.calx) compress at every preset, check that `std.xz` decompresses what they write, and that it's the same on 1 and 3 workers.

## How it's built

Decompressing is [runtime/calx_xz.c](../../runtime/calx_xz.c), linked only into programs that call it, written from the .xz file format's specification and the LZMA algorithm as the public-domain LZMA SDK describes it.

| Part | How |
| --- | --- |
| The range decoder | Kept in registers, with a fast loop while 64 bytes of input are left that doesn't check where the input ends, and a careful one for the rest |
| Bits in trees | Without branches, and with the next level's probabilities loaded before the bit is known |
| Literals after a match | Both of the next probabilities loaded at once |
| Copying a match | 16 bytes at a time when the distance is at least the length, never past its end, so blocks next to each other can decode at the same time |
| CRC-64 | Folded with the processor's carry-less multiply in four lanes, 64 bytes a step, at about 39 GB/s on bytes in the cache, with tables where it's missing |
| CRC-32 and SHA-256 | `std.zlib`'s and `std.hash`'s |

Compressing is [runtime/calx_xzenc.c](../../runtime/calx_xzenc.c), written from the same specification and the LZMA SDK's account of LZMA's encoder. It has no liblzma code.

| Part | How |
| --- | --- |
| The range coder | A 64-bit `low`, whose carry goes into the byte held back before it, and the 0xFF bytes after that |
| Prices | A bit's price from a table of -log2 of 128 probabilities, in sixteenths of a bit. The prices of lengths and distances are kept in tables, worked out again as the probabilities move |
| Hash chains and binary trees | Two-, three- and four-byte hashes. The two- and three-byte ones mix the first byte in the same way, so a byte compared tells the rest. Matches are compared 8 bytes at a time |
| The optimal parser | The cheapest way through the next few thousand bytes, with literals, matches, repeats, short repeats, and a literal and a repeat after a match or a repeat, as the SDK's parser has them |
| Chunks | LZMA2 chunks of up to 2 MiB of text, closed before their code passes 64 KiB, from a bound on the code a parse can take. A chunk that doesn't compress is written as it is |
| Threads | The binary trees shared out by hash, the slices parsed at once from their notes, and each window of 16 MiB of text, or of the dictionary, matched while the one before is parsed |

`std.xz` reads the headers, the index and the footer in Calx over those runtime calls, in the order liblzma checks them, so broken files fail the same way, and writes them for `compress`: [std/xz.calx](../../std/xz.calx).
