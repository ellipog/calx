---
title: std.text.utf8
description: Bytes that may not be UTF-8 as text, with U+FFFD in their place, as Java's new String(bytes, UTF_8) gives them - decode_lossy.
---

# `std.text.utf8`

Bytes that may not be UTF-8, read as text the way Java reads them: with U+FFFD, the replacement character `�`, in place of each piece that isn't UTF-8. [`String.from_utf8`](../reference/strings.md) gives `none` for such bytes instead, which is right where they mean the data is broken; this is for text that must read whatever comes, as a console's lines and Java's strings do.

```calx
import std.text.utf8.{decode_lossy}

fn main() {
    let bytes: List<u8> = [0x73, 0x74, 0xC3, 0xB6, 0x6E, 0x65, 0xFF, 0x21]
    println(decode_lossy(bytes, 0, bytes.len()))    // out: stöne�!
    println(decode_lossy(bytes, 0, 3))              // out: st�
}
```

| Function | Gives |
| --- | --- |
| `decode_lossy(bytes: List<u8>, from: usize, to: usize) -> String` | The bytes from offset `from` up to `to` as text, as Java's `new String(bytes, from, to - from, UTF_8)` gives it. Panics at the call if `from..to` isn't inside `bytes` |

Valid UTF-8 comes through as it is. Which bytes one U+FFFD stands for follows Java's decoder, `sun.nio.cs.UTF_8`, which is mostly Unicode's practice of one for each longest piece that could still start a character, but not always:

| Bytes | Become |
| --- | --- |
| A byte that can't start a character: `80` to `C1`, or `F5` to `FF` | One U+FFFD each |
| A lead byte whose next byte isn't the continuation it needs, as `C3 41`, or makes an overlong form or one past U+10FFFF, as `E0 80`, `F0 8F` or `F4 90` | One for the lead byte, then the next byte is read again |
| A character cut off by a byte that isn't a continuation, or by `to`, as `E2 82 41` or `F0 9F 98` at the end | One for what's there |
| A surrogate, `ED A0 80` to `ED BF BF`, which UTF-8 can't hold | One for all three bytes, since Java decodes it before rejecting it. Unicode's practice would give three |

So `ED A0 80` is one U+FFFD where `E0 80 80`, an overlong form, is three, as in Java.

- Valid text costs one copy of its bytes and one check, the same as `String.from_utf8` of a copy. Only text that isn't valid is gone over again, a byte at a time.
- It was compared with Java 25 on 200,000 random byte strings, made of the bytes where the rules change, characters whole and cut off, and surrogates, with none different: [tools/unicode/utf8_check.py](../../tools/unicode/utf8_check.py), with [Utf8Check.java](../../tools/unicode/Utf8Check.java) as Java's side.
- [`std.io.Lines`](io.md#lines) decodes each line with it.

`std.text.utf8` is written in Calx: [std/text/utf8.calx](../../std/text/utf8.calx).
