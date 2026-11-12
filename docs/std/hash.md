---
title: std.hash
description: MD5, SHA-1, SHA-256, SHA-384 and SHA-512, at once or in pieces, and HMAC with any of them, giving what Java's MessageDigest and Mac and Python's hashlib and hmac give, at or near OpenSSL's speed.
---

# `std.hash`

Common hashes, giving the bytes Java's `MessageDigest` and `Mac` and Python's `hashlib` and `hmac` give:

- MD5, for an offline player's UUID, which `UUID.nameUUIDFromBytes` makes from `OfflinePlayer:<name>`.
- SHA-1, for the server hash a login sends the session server.
- SHA-256, for the hashed seed in the join packet, and with SHA-512 for checking what a script downloads.
- HMAC-SHA256, which Velocity signs the player data it forwards with, and HMAC with any of the others.

On a modern x86-64 processor SHA-1 and SHA-256 run on its SHA instructions, as OpenSSL's do, and every hash is as fast as Python's or close ([below](#speed)).

```calx
import std.bytes.{hex}
import std.hash.{Algorithm, Digest, hmac, hmac_sha256, md5, sha256, sha512}

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
    println(hex(hmac(Algorithm.Sha1, "key".to_bytes(), "The quick brown fox jumps over the lazy dog".to_bytes())))    // out: de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9

    // What a download should hash to, as a release lists it.
    println(hex(sha512("abc".to_bytes()))[..32])    // out: ddaf35a193617abacc417349ae204131
}
```

| Item | Gives |
| --- | --- |
| `md5(bytes: List<u8>) -> List<u8>` | The MD5 of the bytes, 16 of them |
| `sha1(bytes: List<u8>) -> List<u8>` | The SHA-1, 20 bytes |
| `sha256(bytes: List<u8>) -> List<u8>` | The SHA-256, 32 bytes |
| `sha384(bytes: List<u8>) -> List<u8>` | The SHA-384, 48 bytes |
| `sha512(bytes: List<u8>) -> List<u8>` | The SHA-512, 64 bytes |
| `digest(algorithm: Algorithm, bytes: List<u8>) -> List<u8>` | The hash with any of them, as Python's `hashlib.new(name, data).digest()` |
| `file_digest(algorithm: Algorithm, path: String) -> List<u8> ! FsError` | The hash of a file, read a mebibyte at a time so a file of any size takes no more memory than that, as Python's `hashlib.file_digest`. `NotFound` if it isn't there |
| `hmac(algorithm: Algorithm, key: List<u8>, message: List<u8>) -> List<u8>` | The HMAC of the message under the key, as RFC 2104 makes it and Java's `Mac` and Python's `hmac.digest` give it. A key longer than a block is hashed first |
| `hmac_sha256(key: List<u8>, message: List<u8>) -> List<u8>` | The HMAC-SHA256 of the message under the key, 32 bytes, as Java's `Mac.getInstance("HmacSHA256")` gives. A key longer than 64 bytes is hashed first, as the RFC says |
| `Algorithm` | `Md5`, `Sha1`, `Sha256`, `Sha384` or `Sha512` |
| `a.len() -> usize`, `a.block_len() -> usize` | How many bytes its hash is, and how many it hashes at a time: 128 for SHA-384 and SHA-512, 64 for the others |
| `Digest.new(algorithm: Algorithm) -> Digest` | A hash to work out in pieces |
| `d.add(bytes: List<u8>)` | Adds the bytes |
| `d.add_text(text: String)` | Adds the text's UTF-8 bytes, which is what Java's `getBytes(UTF_8)` gives |
| `d.finish() -> List<u8>` | The hash of everything added. It takes the `Digest`, which can't be used after |

- MD5 and SHA-1 are broken as protection against someone choosing what's hashed. `std.hash` has them for matching programs and protocols that use them where that doesn't matter, not for keeping anything safe.
- MD5, SHA-1 and SHA-256 gave the same bytes as Java 25's `MessageDigest` and `Mac` for 3000 random inputs of up to 300 bytes, with keys of 1 to 200 bytes, and with each input also added in two pieces cut at a random place.
- All five gave the same bytes, and the same HMACs, as Python 3.14's `hashlib` and `hmac` for 502 random inputs: every length up to 300 bytes, 200 up to 5000, and two of a MiB, each also in two pieces, with the first piece as the key. The SHA instructions and the plain code were each checked.
- The [tests](../../tests/std/hash.calx) hold Java's and Python's hashes for the lengths around where the padding takes another block.
- To turn a hash into the hex Mojang's session server takes, which is the hash as a signed number, a program does what Java's `new BigInteger(hash).toString(16)` does: the bytes are two's complement, so a hash whose first byte is 0x80 or more is written as `-` and its negation.

## How it's built

[runtime/calx_digest.c](../../runtime/calx_digest.c), written from RFC 1321 (MD5), FIPS 180-4 (SHA-1 and SHA-2) and RFC 2104 (HMAC). The constants are worked out from their definitions: MD5's from sines, SHA-2's from the square and cube roots of the first primes. It's linked only into programs that call it.

- **SHA-1 and SHA-256** use the processor's SHA instructions where `cpuid` says it has them, which most x86-64 processors made since 2017 do: four rounds an instruction. The plain code is the fallback. `cpuid` is asked once per program, since under a hypervisor, as Windows with virtualization-based security runs, each one costs about a microsecond, which made hashing 50 bytes 8 times slower when it was asked every time.
- **MD5** is written out round by round. Each round adds its message word and constant before the function of the round before's result, and an empty `asm` keeps clang from moving the constant back onto that chain.
- **SHA-512 and SHA-384** compute the message schedule two words at a time in SSE2's 64-bit lanes, sixteen rounds ahead, inside the round loop, so it fills the gaps in the rounds' chain. Where the processor has AVX2, they work out two blocks' schedules at once, one block in each half of a 256-bit register, as OpenSSL does: during the first block's rounds, after which the second block's rounds run on words already made. Each round sums its parts in the order they're ready, with the same empty `asm` as MD5's holding each partial sum where it is, so each round waits on the one before only for Sigma1 and an add. On the AVX2 path, which has BMI too, the majority function is `(a & bxc) + (b & ~bxc)`, where `bxc` is `b ^ c`, the round before's `a ^ b`: where `b` and `c` differ the majority is `a`'s bit, and where they agree it's `b`'s. The second half doesn't wait on `a` and is one `andn`, and it takes two moves a round fewer than carrying `b | c` and `b & c`, which the other paths still do. The first block's first 64 rounds are a loop of their own, so the schedule's vector work mixes in with the rounds' instead of coming all at once, as it did behind an `i < 64` test in the loop.
- A `Digest` holds its state in a `List<u8>` of 224 bytes, which the runtime copies into a struct and back, so it needs no alignment and `std.hash` needs no unsafe types.
- Whole blocks are hashed straight from the caller's bytes. Only a part block at the end of an `add` is copied, to wait for the next.
- `hmac` is written in Calx over `Digest`. `hmac_sha256`, which Calcite checks Velocity's forwarding with, is one runtime call.

## Speed

On our benchmark, hashing 64 MiB at once on one P-core of an i9-13900K, the best of ten runs, the two taking turns, against Python 3.14's `hashlib`, which is OpenSSL 3.5. The desktop had a game running on every core, so the numbers are rough.

| Hash | Calx | Python |
| --- | --- | --- |
| MD5 | 1019 MiB/s | 1037 MiB/s |
| SHA-1 | 2756 MiB/s | 2731 MiB/s |
| SHA-256 | 2431 MiB/s | 2302 MiB/s |
| SHA-512 | 1081 MiB/s | 1118 MiB/s |

SHA-512 is the one Python is still ahead on, by about 3%. Its row was taken again for CLX-151 on a quiet machine, best of six rounds of ten, where the runtime before it did 1032 MiB/s. In WSL on the same P-core, Calx does about 1070 MiB/s to `hashlib`'s 1110 (Python 3.14, OpenSSL 3.5.5). CLX-151 took it from about 12% behind, with the two-block schedule, the rounds' sums in order, the `andn` majority and the first block's schedule mixed in with its rounds. What's left is in how the instructions are scheduled: on this core a round with no schedule work takes 7 cycles, though its chain of waits is 4 long, and OpenSSL's rounds are assembly ordered by hand. Before CLX-149, Calx did MD5 at 635 MiB/s, SHA-1 at 600 and SHA-256 at 400.

`std.hash` is written in Calx over those runtime calls: [std/hash.calx](../../std/hash.calx).
