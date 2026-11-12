---
title: std.bytes
description: Bytes as hex and base64 and back, finding bytes in bytes, and numbers read from and written to bytes in either byte order.
---

# `std.bytes`

Bytes as text and text as bytes, finding bytes in bytes, and numbers in bytes, as binary files and network protocols lay them out. It's what Python's `bytes.hex`, `bytes.fromhex`, `base64`, `bytes.find` and `struct` do.

```calx
import std.bytes

fn main() {
    let data: List<u8> = [137, 80, 78, 71, 0, 0, 1, 0]
    println(bytes.hex(data))                       // out: 89504e4700000100
    println("{bytes.u32_be(data, 4)}")              // out: 256
    println(bytes.base64([77u8, 97, 110]))          // out: TWFu
    println("{bytes.from_base64("TWE=")}")          // out: some([77, 97])
    println("{bytes.find(data, [78u8, 71])}")       // out: some(2)
    var out: List<u8> = []
    bytes.put_u16_le(mut out, 513)
    bytes.put_f32_be(mut out, 1.0)
    println("{out}")                                // out: [1, 2, 63, 128, 0, 0]
}
```

## Text

| Function | Gives |
| --- | --- |
| `hex(bytes)` | Lowercase hex digits, two for each byte |
| `from_hex(text)` | The bytes, from hex digits in either case, or `none` when the text has an odd number of them or anything else |
| `base64(bytes)` | Base64 with `+`, `/` and `=` padding, as MIME, PEM and data URLs write it (RFC 4648) |
| `from_base64(text)` | The bytes, with or without the padding, or `none` |
| `base64_url(bytes)` | URL-safe base64, with `-` and `_` and no padding, as JSON Web Tokens and URLs write it |
| `from_base64_url(text)` | The bytes, with or without padding, or `none` |

- Decoding is strict: anything outside the alphabet, padding where it can't be, or bits left over that don't make a whole byte give `none`, so each text means one list of bytes. Python's `b64decode` drops characters it doesn't know unless told to check.
- The text functions take a `String` that they only read, so a `str` literal works too.

## Finding

| Function | Gives |
| --- | --- |
| `find(haystack, needle, from: 0)` | Where `needle` first appears, from index `from`, or `none`. An empty needle is found at `from` |
| `find_byte(haystack, b, from: 0)` | Where the byte `b` first is, from index `from`, or `none`, without making a list for it |

```calx
import std.bytes

fn main() {
    let data: List<u8> = [104, 105, 10, 116, 104, 101, 114, 101, 10]
    println("{bytes.find_byte(data, 10)} {bytes.find_byte(data, 10, from: 3)}")   // out: some(2) some(8)
    println("{bytes.find(data, [104u8, 101])} {bytes.find(data, [33u8])}")        // out: some(4) none
}
```

- They search as a `String`'s `find` does: `memchr` for one byte, and for a longer needle, 32 places tested at once with AVX2, or 16 with SSE2 or NEON, for two of its rarer bytes, so only where both are there is the rest compared. The language benchmarks' reverse-complement finds its line ends with `find_byte` ([`bench/suite/revcomp`](../../bench/suite/revcomp)).

## Numbers

Each fixed-width integer and float, in either byte order:

| Function | Does |
| --- | --- |
| `u32_le(b, at)`, `u32_be(b, at)` | Reads the `u32` in the 4 bytes from index `at`, little- or big-endian |
| `put_u32_le(mut out, v)`, `put_u32_be` | Adds `v`'s bytes at the end of `out` |
| `set_u32_le(mut out, at, v)`, `set_u32_be` | Writes `v`'s bytes over those from index `at` |

- They're there for `u16`, `u32`, `u64`, `i16`, `i32`, `i64`, `f32` and `f64`, so `i64_be`, `put_f64_le` and `set_u16_be` work the same way. Floats are their IEEE 754 bits.
- Little-endian, the low byte first, is how x86 and ARM hold numbers, and how zip and most file formats store them. Big-endian, the high byte first, is network byte order, and Java's `DataOutputStream`, PNG and Minecraft's protocol use it.
- Reading or setting past the end panics, and the panic names the line that called, as `@track_caller` does ([panics](../reference/printing.md#panics-that-name-the-caller)).
- Each checks the range once, then reads or writes a byte at a time with shifts, which clang merges into one load or store, with a byte swap for the order the processor doesn't use. On x86-64, `u64_be` compiles to a compare, a load and a `bswap`.
