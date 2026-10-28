---
title: std.crypto
description: Random bytes from the system, AES-128 in CFB8 mode, and RSA with PKCS#1 v1.5 padding and signatures - what vanilla's online-mode login and signed chat use.
---

# `std.crypto`

What vanilla's online-mode login is made of:

1. The server makes a 1024-bit RSA key pair, and sends the public key in its encryption request.
2. The client makes a random 16-byte secret, encrypts it with the public key, and sends it back.
3. The server decrypts the secret, and from then on both sides encrypt every byte with AES-128 in CFB8 mode, using the secret as the key and the IV.

Each gives what Java's `Cipher`, `KeyPairGenerator` and `SecureRandom` give for the same inputs, so a vanilla client talks to a Calx server as to a Java one.

Signed chat uses RSA signatures. Mojang's key signs each player's key, and the player's key signs each of their messages. The server checks both as Java's `Signature` checks `SHA1withRSA` and `SHA256withRSA`.

```calx
import std.crypto.{Cfb8, RsaKey, random_bytes}

fn main() {
    // The server's key, and the public half it sends.
    let key = RsaKey.generate(1024)
    println("{key.size()} {key.public_der().len()}")    // out: 128 162

    // The client's secret, sealed with the public key, and opened by the server.
    let secret = random_bytes(16)
    let sealed = key.encrypt(secret.copy())!
    let opened = key.decrypt(sealed)!
    println("{opened.len()} {opened[0] == secret[0]}")    // out: 16 true

    // Both sides make a cipher from the secret, one for each direction.
    var out = Cfb8.new(secret.copy(), secret.copy())
    var back = Cfb8.new(opened.copy(), opened.copy())
    var packet = "Hello, Calcite!".to_bytes()
    out.encrypt(mut packet)
    back.decrypt(mut packet)
    println(String.from_utf8(packet) ?? "bad")    // out: Hello, Calcite!
}
```

```calx
import std.crypto.{RsaKey, RsaPublicKey}
import std.hash.{Algorithm}

fn main() {
    // A player's key signs a message, and the server checks it with the public key it was sent.
    let player = RsaKey.generate(2048)
    let message = "hello".to_bytes()
    let signature = player.sign(Algorithm.Sha256, message.copy())
    let key = RsaPublicKey.from_der(player.public_der())!
    println("{signature.len()} {key.verify(Algorithm.Sha256, message.copy(), signature.copy())}")    // out: 256 true
    println("{key.verify(Algorithm.Sha256, "hellO".to_bytes(), signature)}")    // out: false
}
```

| Item | Gives |
| --- | --- |
| `random_bytes(n: usize) -> List<u8>` | `n` bytes from the system's generator, good for keys, secrets and tokens |
| `Cfb8.new(key: List<u8>, iv: List<u8>) -> Cfb8` | AES-128 in CFB8 mode, as Java's `Cipher.getInstance("AES/CFB8/NoPadding")`. Panics unless both are 16 bytes |
| `c.encrypt(bytes: mut List<u8>)` | Encrypts the bytes where they are, carrying on the stream from the last call |
| `c.decrypt(bytes: mut List<u8>)` | Decrypts the bytes where they are, carrying on the stream from the last call |
| `RsaKey.generate(bits: u32) -> RsaKey` | A new key pair, `bits` 1024 or 2048, with 65537 as the public exponent, as Java's `KeyPairGenerator` makes. Panics for any other size |
| `k.primes() -> List<u8>` | The key pair's two primes, each half the modulus's bytes, big-endian: 128 bytes for 1024 bits. Anyone with them has the private key |
| `RsaKey.from_primes(primes: List<u8>) -> RsaKey?` | The key pair whose `primes` these are, the same as the one that gave them, such as a server's key restored to replay a session. `none` unless they're two different primes as `generate` makes them, their top two bits set, 512 or 1024 bits each. Checking that they're prime takes 1.5 ms for a 1024-bit key and 11 ms for 2048 |
| `k.public_der() -> List<u8>` | The public key as Java's `getEncoded()` gives it, an X.509 SubjectPublicKeyInfo in DER: 162 bytes for 1024 bits, 294 for 2048 |
| `k.encrypt(message: List<u8>) -> List<u8>?` | The message encrypted with the public key and random padding, as Java's `"RSA/ECB/PKCS1Padding"`: `k.size()` bytes. `none` if the message is longer than `k.size() - 11` |
| `k.decrypt(bytes: List<u8>) -> List<u8>?` | The message inside, or `none` if the bytes aren't `k.size()` long or their padding is wrong |
| `k.size() -> usize` | The modulus's length in bytes: 128 for 1024 bits |
| `k.sign(hash: Algorithm, message: List<u8>) -> List<u8>` | The PKCS#1 v1.5 signature of the message with its `hash`, as Java's `Signature` signs with `"SHA256withRSA"` for `Algorithm.Sha256`, `"SHA1withRSA"` for `Sha1` and `"MD5withRSA"` for `Md5`: `k.size()` bytes, the same each time |
| `k.public_key() -> RsaPublicKey` | The public key, which checks what `k` signs |
| `RsaPublicKey.from_der(der: List<u8>) -> RsaPublicKey?` | The key in X.509 DER, as Java's `getEncoded()` writes one, or `none` if it isn't one: every length and number in its shortest form, nothing after it, `rsaEncryption` with no parameters, an odd modulus of 512 to 4096 bits, and an exponent from 3 to below the modulus |
| `p.verify(hash: Algorithm, message: List<u8>, signature: List<u8>) -> bool` | Whether the signature is the key's PKCS#1 v1.5 signature of the message with its `hash`, as Java's `Signature.verify` decides. One that's the modulus or more is no signature, as Java decides too, and nor is one that isn't `p.size()` bytes, where Java throws a `SignatureException`: `Bad signature length: got 255 but was expecting 256` |
| `p.der() -> List<u8>`, `p.size() -> usize` | The key's DER, as it was read, which is what `getEncoded()` gives, and its modulus's length in bytes |

- CFB8 is a stream cipher: each byte encrypts to one byte, and how a stream is split into calls doesn't change what comes out. A connection keeps one `Cfb8` for each direction for as long as it lasts.
- `decrypt` says nothing about why bytes were refused, and takes the same time either way, so a client sending made-up bytes learns nothing about the key from the answers (Bleichenbacher's attack).
- The padding is random, so the same message encrypts to different bytes each time. A signature's isn't, so the same message signs the same.
- `from_der` takes keys of up to 4096 bits, Mojang's size; Java's `KeyFactory` takes up to 16384.

## Checked against Java

- AES-CFB8: 300 random streams of up to 699 bytes, encrypted in random pieces, gave the same bytes as Java 25's `Cipher` gave for each whole stream, and decrypting them in other random pieces gave them back. The portable version gave the same as AES-NI on 2000 more.
- RSA: Java decoded `public_der` with `KeyFactory`, encrypted 200 random messages of 0 to 117 bytes with `Cipher`, and `decrypt` opened every one, for 1024 and 2048 bits. Java encoded the key it decoded to the same bytes. Bytes with broken padding gave `none`.
- Signatures: Java 25 made 1024-, 2048- and 4096-bit keys and signed a message with `SHA1withRSA` and `SHA256withRSA`; `verify` takes each, and not with the other hash, another message, or a bit flipped. Java verified what `sign` signed with both, for 1024 and 2048 bits.
- The [tests](../../tests/std/crypto.calx) hold Java's bytes for one AES stream, and check the rest by round trips and shapes. [Those of signatures](../../tests/std/signatures.calx) hold Java's keys and signatures.

## How it's built

[runtime/calx_crypto.c](../../runtime/calx_crypto.c), linked only into programs that call it, written from FIPS 197 (AES), NIST SP 800-38A (CFB8) and RFC 8017 (RSA and its padding).

| Part | How |
| --- | --- |
| Random bytes | `BCryptGenRandom` on Windows, `getrandom` on Linux |
| AES | AES-NI where the processor has it, which `cpuid` tells when a `Cfb8` is made. Otherwise a byte-at-a-time version, with the S-box worked out from its definition |
| CFB8 encrypting | One AES block for each byte, since each byte's input is the one before it. That's 16 times the work of the block modes, and it's why this mode is slow everywhere |
| CFB8 decrypting | The inputs are the ciphertext, which is all there already, so it runs 16 blocks at once through AES-NI's pipeline |
| RSA numbers | 64-bit limbs, and Montgomery multiplication with a final subtraction that takes the same time whether it's needed or not |
| RSA private key | Decrypting and signing use the Chinese remainder theorem: two exponents of half the length. Exponents go four bits a step from a table that's read whole each time, so neither the time nor the memory touched depends on the key's bits |
| RSA public key | Read once, with what Montgomery multiplication needs worked out then: R^2 mod n, as 2^k R doubled up from the top power of 2 below n and squared 6 times, so about 64 doublings where 1 doubled 128 times a limb takes 4096 for 2048 bits. The exponent isn't secret, so it goes a bit at a time, a squaring each and a multiply for each that's set: 65537 takes 17 |
| Primes | 300 small primes, then 16 rounds of Miller-Rabin, which a composite passes with odds below 2^-32 |

On an i9-13900K, one run each:

| | Calx | Java 25 |
| --- | --- | --- |
| CFB8 encrypting | 135 MB/s | 77 MB/s |
| CFB8 decrypting | 659 MB/s | 79 MB/s |
| A 1024-bit key pair | 8.4 ms | 7 to 16 ms |
| A 1024-bit decrypt | 97 µs | 108 µs |
| A 2048-bit `SHA256withRSA` verify | 19 µs | 22 µs |

The ciphers run for every byte a player sends or gets, so they're what counts. RSA runs once for each login, and a verify once for each signed chat message. Java's verify is its best of three runs of 2000, after its JIT has warmed up. Interleaving the multiply's two carry chains with ADX would make RSA about twice as fast again.

`std.crypto` is written in Calx over those runtime calls: [std/crypto.calx](../../std/crypto.calx).
