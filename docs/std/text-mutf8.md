---
title: std.text.mutf8
description: Java's Modified UTF-8, which NBT uses - encode, encoded_len and decode, matching writeUTF and readUTF.
---

# `std.text.mutf8`

Java's Modified UTF-8, the encoding of NBT's strings and of Java's `DataOutput.writeUTF`. It's UTF-8 except in two ways:

- U+0000 is the two bytes `C0 80`, so the encoded text never holds a zero byte.
- A rune past U+FFFF is its two UTF-16 surrogates, three bytes each, since Java's text is UTF-16 units. That's six bytes where UTF-8 takes four.

```calx
import std.text.mutf8.{DecodeError, decode, encode, encoded_len}

fn main() {
    let name = String("Steve \u{1F600}")
    var out = List<u8>()
    encode(name, mut out)
    println("{name.len()} {encoded_len(name)} {out.len()}")    // out: 10 12 12

    match decode(out, 0, out.len()) {
        ok(back) => println("{back == name}"),    // out: true
        err(e) => println("{e}"),
    }
    let bad: List<u8> = [0x61, 0xC3]
    match decode(bad, 0, bad.len()) {
        ok(s) => println(s),
        err(DecodeError.Truncated) => println("cut off"),    // out: cut off
        err(DecodeError.Malformed(at)) => println("bad byte at {at}"),
    }
}
```

| Item | Gives |
| --- | --- |
| `encoded_len(s: String) -> usize` | How many bytes `s` takes: its UTF-8 length, plus one for each U+0000 and two for each rune past U+FFFF |
| `encode(s: String, out: mut List<u8>)` | Adds `s` to the end of `out`, as `writeUTF` writes it after its two-byte length |
| `decode(bytes: List<u8>, from: usize, to: usize) -> String ! DecodeError` | The text in `bytes` from offset `from` up to `to`, as `readUTF` reads it. Panics if `from..to` isn't inside `bytes` |
| `DecodeError` | `Truncated`, when the bytes end inside a character, or `Malformed(at)`, with the offset of a byte that can't start a character or isn't the continuation one needs |

- `decode` accepts exactly what Java's `readUTF` accepts: a plain zero byte, and longer forms than needed, such as `C1 81` for `A`. It rejects four-byte UTF-8, which Java writes as two surrogates.
- A surrogate without its pair is text Java can hold but UTF-8 can't, so `decode` makes it `?`. That's what Java writes for one in UTF-8, as the protocol does.
- `decode` takes offsets, so it reads a string where it sits in a packet or a file, with nothing copied out first.
- The length prefix isn't part of it, since NBT and the protocol each write their own.
- `encode` and `encoded_len` matched Java 25's `writeUTF` on 20,000 random strings, and `decode` matched `readUTF` on 30,000 random byte strings, 13,628 of them valid, accepting and rejecting the same ones.

`std.text.mutf8` is written in Calx, over [`s.chars()`](../reference/strings.md#going-over-the-text): [std/text/mutf8.calx](../../std/text/mutf8.calx).
