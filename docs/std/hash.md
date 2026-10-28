---
title: std.hash
description: MD5, SHA-1 and SHA-256, at once or in pieces, and HMAC-SHA256, giving what Java's MessageDigest and Mac give.
---

# `std.hash`

The hashes vanilla and its proxies use, giving the bytes Java's `MessageDigest` and `Mac` give:

- MD5, for an offline player's UUID, which `UUID.nameUUIDFromBytes` makes from `OfflinePlayer:<name>`.
- SHA-1, for the server hash a login sends the session server.
- SHA-256, for the hashed seed in the join packet.
- HMAC-SHA256, which Velocity signs the player data it forwards with.

```calx
import std.hash.{Algorithm, Digest, hmac_sha256, md5, sha256}

fn hex(bytes: List<u8>) -> String {
    let digits = "0123456789abcdef"
    var s = String()
    for b in bytes {
        let hi = usize(b / 16)
        let lo = usize(b % 16)
        s.push_str(digits[hi..hi + 1])
        s.push_str(digits[lo..lo + 1])
    }
    return s
}

fn main() {
    println(hex(md5("OfflinePlayer:Notch".to_bytes())))    // out: b50ad385829da141a2167e7d7539ba7f
    println(hex(sha256("abc".to_bytes())))    // out: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

    // In pieces, which give the hash of them all at once.
    var d = Digest.new(Algorithm.Md5)
    d.add_text("OfflinePlayer:")
    d.add_text("Notch")
    println(hex(d.finish()))    // out: b50ad385829da141a2167e7d7539ba7f

    let mac = hmac_sha256("key".to_bytes(), "The quick brown fox jumps over the lazy dog".to_bytes())
    println(hex(mac))    // out: f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
}
```

| Item | Gives |
| --- | --- |
| `md5(bytes: List<u8>) -> List<u8>` | The MD5 of the bytes, 16 of them |
| `sha1(bytes: List<u8>) -> List<u8>` | The SHA-1, 20 bytes |
| `sha256(bytes: List<u8>) -> List<u8>` | The SHA-256, 32 bytes |
| `hmac_sha256(key: List<u8>, message: List<u8>) -> List<u8>` | The HMAC-SHA256 of the message under the key, 32 bytes, as Java's `Mac.getInstance("HmacSHA256")` gives. A key longer than 64 bytes is hashed first, as the RFC says |
| `Algorithm` | `Md5`, `Sha1` or `Sha256` |
| `Digest.new(algorithm: Algorithm) -> Digest` | A hash to work out in pieces |
| `d.add(bytes: List<u8>)` | Adds the bytes |
| `d.add_text(text: String)` | Adds the text's UTF-8 bytes, which is what Java's `getBytes(UTF_8)` gives |
| `d.finish() -> List<u8>` | The hash of everything added. It takes the `Digest`, which can't be used after |

- MD5 and SHA-1 are broken as protection against someone choosing what's hashed. Vanilla uses them where that doesn't matter, so `std.hash` has them for matching it, not for keeping anything safe.
- They gave the same bytes as Java 25's `MessageDigest` and `Mac` for 3000 random inputs of up to 300 bytes, with keys of 1 to 200 bytes, and with each input also added in two pieces cut at a random place. The [tests](../../tests/std/hash.calx) hold Java's hashes for the lengths around where the padding takes another block.
- To turn a hash into the hex Mojang's session server takes, which is the hash as a signed number, Calcite does what Java's `new BigInteger(hash).toString(16)` does: the bytes are two's complement, so a hash whose first byte is 0x80 or more is written as `-` and its negation.

## How it's built

[runtime/calx_digest.c](../../runtime/calx_digest.c), written from RFC 1321 (MD5), FIPS 180-4 (SHA-1 and SHA-256) and RFC 2104 (HMAC). The constants are worked out from their definitions: MD5's from sines, SHA-256's from the square and cube roots of the first primes. It's linked only into programs that call it.

- A `Digest` holds its state in a `List<u8>` of 112 bytes, which the runtime copies into a struct and back, so it needs no alignment and `std.hash` needs no unsafe types.
- Each 64-byte block is hashed straight from the caller's bytes. Only a part block at the end of an `add` is copied, to wait for the next.

`std.hash` is written in Calx over those runtime calls: [std/hash.calx](../../std/hash.calx).
