---
title: std.crypto
description: Random bytes from the system, AES-128 in CFB8 mode, RSA with PKCS#1 v1.5 padding and signatures - what Minecraft's online-mode login and signed chat use - and argon2id password hashing in PHC strings, Ed25519 signatures, and authenticated encryption with ChaCha20-Poly1305, XChaCha20-Poly1305 and AES-GCM, as Python's argon2-cffi and cryptography give them.
---

# `std.crypto`

What Minecraft's online-mode login is made of:

1. The server makes a 1024-bit RSA key pair, and sends the public key in its encryption request.
2. The client makes a random 16-byte secret, encrypts it with the public key, and sends it back.
3. The server decrypts the secret, and from then on both sides encrypt every byte with AES-128 in CFB8 mode, using the secret as the key and the IV.

Each gives what Java's `Cipher`, `KeyPairGenerator` and `SecureRandom` give for the same inputs, so a Java program on the other end of a connection gets what it expects.

Signed chat uses RSA signatures. Mojang's key signs each player's key, and the player's key signs each of their messages. The server checks both as Java's `Signature` checks `SHA1withRSA` and `SHA256withRSA`.

For everything else there are [passwords](#passwords), hashed with argon2id into the PHC strings Python's argon2-cffi writes, [Ed25519](#ed25519) signatures, which minisign and SSH keys are, as Python's `cryptography` makes and checks them, and [authenticated encryption](#authenticated-encryption), ChaCha20-Poly1305, XChaCha20-Poly1305 and AES-GCM, for keeping secrets that only a key opens and that can't be changed unnoticed, as `cryptography` seals them.

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
    var packet = "Hello, Calx!".to_bytes()
    out.encrypt(mut packet)
    back.decrypt(mut packet)
    println(String.from_utf8(packet) ?? "bad")    // out: Hello, Calx!
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
| `k.sign(hash: Algorithm, message: List<u8>) -> List<u8>` | The PKCS#1 v1.5 signature of the message with its `hash`, as Java's `Signature` signs with `"SHA256withRSA"` for `Algorithm.Sha256`, `"SHA1withRSA"` for `Sha1`, `"MD5withRSA"` for `Md5`, and `"SHA384withRSA"` and `"SHA512withRSA"` for `Sha384` and `Sha512`: `k.size()` bytes, the same each time |
| `k.public_key() -> RsaPublicKey` | The public key, which checks what `k` signs |
| `RsaPublicKey.from_der(der: List<u8>) -> RsaPublicKey?` | The key in X.509 DER, as Java's `getEncoded()` writes one, or `none` if it isn't one: every length and number in its shortest form, nothing after it, `rsaEncryption` with no parameters, an odd modulus of 512 to 4096 bits, and an exponent from 3 to below the modulus |
| `p.verify(hash: Algorithm, message: List<u8>, signature: List<u8>) -> bool` | Whether the signature is the key's PKCS#1 v1.5 signature of the message with its `hash`, as Java's `Signature.verify` decides. One that's the modulus or more is no signature, as Java decides too, and nor is one that isn't `p.size()` bytes, where Java throws a `SignatureException`: `Bad signature length: got 255 but was expecting 256` |
| `p.der() -> List<u8>`, `p.size() -> usize` | The key's DER, as it was read, which is what `getEncoded()` gives, and its modulus's length in bytes |

- CFB8 is a stream cipher: each byte encrypts to one byte, and how a stream is split into calls doesn't change what comes out. A connection keeps one `Cfb8` for each direction for as long as it lasts.
- `decrypt` says nothing about why bytes were refused, and takes the same time either way, so a client sending made-up bytes learns nothing about the key from the answers (Bleichenbacher's attack).
- The padding is random, so the same message encrypts to different bytes each time. A signature's isn't, so the same message signs the same.
- `from_der` takes keys of up to 4096 bits, Mojang's size; Java's `KeyFactory` takes up to 16384.

## Passwords

A password is stored as its argon2id hash, as RFC 9106 makes it and Python's argon2-cffi does: a random salt, then a pass over 64 MiB of memory three times, so each guess costs someone with a copy of the hashes that memory and about 65 ms of a core's time. The hash is a PHC string, `$argon2id$v=19$m=65536,t=3,p=4$<salt>$<hash>`, which holds its parameters and salt, so it's all a database keeps, and argon2-cffi, libsodium and the reference implementation read it as `std.crypto` does.

```calx
import std.crypto.{PasswordHasher, verify_password}

fn main() {
    // argon2-cffi's PasswordHasher(): 64 MiB, 3 passes, 4 lanes. The test asks for less.
    let ph = PasswordHasher { memory_kib: 1024, iterations: 2, parallelism: 2 }
    let stored = ph.hash("hunter2")
    println(stored[..30])    // out: $argon2id$v=19$m=1024,t=2,p=2$

    println("{verify_password(stored, "hunter2")} {verify_password(stored, "hunter3")}")    // out: true false

    // A login whose hash was made with other parameters makes it again from the password.
    let now = PasswordHasher()
    if verify_password(stored, "hunter2") and now.needs_rehash(stored) {
        println("rehash")    // out: rehash
    }

    // The reference implementation's hash of "password" with the salt "somesalt".
    println("{verify_password("$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8ArZWb2GRPPc", "password")}")    // out: true
}
```

```calx
import std.bytes.{hex}
import std.crypto.{Argon2Type, argon2}

fn main() {
    // argon2.low_level.hash_secret_raw(b"password", b"somesalt", time_cost=2, memory_cost=256,
    // parallelism=2, hash_len=24, type=Type.ID)
    let tag = argon2("password".to_bytes(), "somesalt".to_bytes(), memory_kib: 256, iterations: 2, parallelism: 2, len: 24)
    println(hex(tag))    // out: e5c9a33c83899d2f5c89bb9da84f83377a0290539f87632f

    // RFC 9106's argon2d vector: a secret and associated data mixed in.
    let d = argon2([1; 32], [2; 16], memory_kib: 32, iterations: 3, parallelism: 4, secret: [3; 8], data: [4; 12], kind: Argon2Type.D)
    println(hex(d)[..16])    // out: 512b391b6f116297
}
```

| Item | Gives |
| --- | --- |
| `PasswordHasher` | `memory_kib: u32 = 65536`, `iterations: u32 = 3`, `parallelism: u32 = 4`, `hash_len: usize = 32` and `salt_len: usize = 16`, argon2-cffi's defaults, RFC 9106's second recommendation. `PasswordHasher()` takes them all; `PasswordHasher { memory_kib: 19456, iterations: 2, parallelism: 1 }` changes some |
| `ph.hash(password: String) -> String` | The password's argon2id hash with a new salt from the system's random source, as a PHC string, as argon2-cffi's `ph.hash(password)`. Panics if the parameters are out of argon2's range, as `argon2` says |
| `verify_password(hash: String, password: String) -> bool` | Whether the password is the one the PHC string was made from, with the string's own parameters, so it takes hashes made with any. It takes argon2id, argon2i and argon2d of version 19, and compares in constant time. `false` for a string that isn't a hash, where argon2-cffi's `ph.verify` raises |
| `ph.needs_rehash(hash: String) -> bool` | Whether the hash was made with another type, version, memory, passes, lanes, salt length or hash length than `ph` would use now, as argon2-cffi's `check_needs_rehash`; `true` for one that isn't a hash |
| `argon2(password: List<u8>, salt: List<u8>, memory_kib: u32 = 65536, iterations: u32 = 3, parallelism: u32 = 4, len: usize = 32, secret: List<u8> = [], data: List<u8> = [], kind: Argon2Type = Argon2Type.Id) -> List<u8>` | The raw tag, `len` bytes, as argon2-cffi's `hash_secret_raw`, with RFC 9106's secret key and associated data. Panics unless the salt is 8 bytes or more, `len` 4 or more, `iterations` 1 or more, `parallelism` 1 to 16777215 and `memory_kib` at least 8 for each lane, or if the memory can't be had |
| `Argon2Type` | `D`, `I` or `Id`; `t.name()` is `argon2d`, `argon2i` or `argon2id` |
| `compare_digest(a: List<u8>, b: List<u8>) -> bool` | Whether they're the same bytes, in a time that depends only on their lengths, as Python's `hmac.compare_digest`: for checking tokens and MACs without telling someone timing it how much of a guess was right |

- Hashing takes about 65 ms with the defaults on a desktop's core, on purpose. A server hashes on a worker thread from [`std.thread`](thread.md), so its other requests carry on.
- The lanes of each slice of memory run at once, each on a thread of its own, up to 16, and meet at the slice's end, as the reference implementation's threads do; the result is the same as running them one after another. With `parallelism: 1` it's one thread, about 145 ms for 64 MiB and 3 passes.
- `memory_kib` that isn't a multiple of 4 lanes is rounded down to one, as the RFC says.
- `verify_password` works out what the string asks for, so a string from someone else asking for a terabyte makes the program try to have it. Hashes come from a program's own database.
- The memory is zeroed before it's freed, and so is what the password was hashed into.
- The [tests](../../tests/std/argon2.calx) hold RFC 9106's vectors for all three types, and the reference implementation's PHC strings for argon2i and argon2id, which OpenSSL 3.5's `ARGON2I` and `ARGON2ID` KDFs give too, as they do for the other tags there.

## Ed25519

Ed25519 signatures, as RFC 8032 makes them and Python's `cryptography` signs and checks them: a key is a 32-byte seed, its public key 32 bytes, and a signature 64. They're what minisign, signify, SSH's `ssh-ed25519` and most signed updates use, and the same message always signs the same.

```calx
import std.bytes.{from_hex, hex}
import std.crypto.{Ed25519Key, Ed25519PublicKey}

fn main() {
    // RFC 8032's first test: its seed, public key and signature of the empty message.
    let key = Ed25519Key.from_seed(from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")!)!
    println(hex(key.public_key().bytes()))    // out: d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
    let sig = key.sign([])
    println(hex(sig.copy())[..32])    // out: e5564300c360ac729086e2cc806e828a

    // What a program that checks an update holds: the public key's bytes.
    let public = Ed25519PublicKey.from_bytes(from_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")!)!
    println("{public.verify([], sig.copy())} {public.verify([0], sig)}")    // out: true false

    let fresh = Ed25519Key.generate()
    let note = "release 1.2".to_bytes()
    println("{fresh.public_key().verify(note.copy(), fresh.sign(note.copy()))}")    // out: true
}
```

| Item | Gives |
| --- | --- |
| `Ed25519Key.generate() -> Ed25519Key` | A new key from 32 bytes of the system's random source, as `Ed25519PrivateKey.generate()` |
| `Ed25519Key.from_seed(seed: List<u8>) -> Ed25519Key?` | The key with that seed, as `Ed25519PrivateKey.from_private_bytes(seed)`; `none` unless it's 32 bytes, all of which are seeds |
| `k.seed() -> List<u8>` | The 32-byte seed, as `private_bytes_raw()`. Anyone with it can sign as the key |
| `k.public_key() -> Ed25519PublicKey` | The public key, which checks what `k` signs |
| `k.sign(message: List<u8>) -> List<u8>` | The 64-byte signature, as `k.sign(message)` gives it |
| `Ed25519PublicKey.from_bytes(bytes: List<u8>) -> Ed25519PublicKey?` | The key in 32 bytes, as `Ed25519PublicKey.from_public_bytes(bytes)`; `none` unless they're a point on the curve in its one encoding |
| `p.bytes() -> List<u8>` | The key's 32 bytes, as `public_bytes_raw()` |
| `p.verify(message: List<u8>, signature: List<u8>) -> bool` | Whether the signature is the key's of the message, as `cryptography`'s `verify` decides, where it raises `InvalidSignature` for no. One that isn't 64 bytes isn't |

- `verify` decides as OpenSSL, which `cryptography` is, and Go decide: S must be below the group's order L, so no second form of a signature passes, the key must be a point in its one encoding, and [S]B − [k]A must encode to the signature's R, byte for byte.
- A minisign signature is Ed25519 of the file's BLAKE2b-512, `blake2b(bytes)` from [`std.hash`](hash.md#blake2b), for its prehashed `ED` form, or of the file itself for the legacy `Ed`.
- There's no X25519 key exchange yet.
- The [tests](../../tests/std/ed25519.calx) hold RFC 8032's five Ed25519 vectors, each signed and verified, and not verified with a bit flipped in R or S or a byte added to the message, then keys that aren't points or aren't in their one encoding, and an S of L more than a good one.

## Authenticated encryption

Each of the three seals a message with a key and a nonce: the ciphertext, as long as the message, with a 16-byte tag after it. Opening it takes the same key and nonce, and gives the message back only if the tag matches, so a sealed value that someone changed, or moved, or made up, opens as `none` rather than as other bytes. Associated data, such as the id of the record a secret is stored in, isn't encrypted or put in the sealed bytes, but the tag covers it, so the secret opens only as that record's. They're RFC 8439's ChaCha20-Poly1305, its XChaCha20-Poly1305 variant from draft-irtf-cfrg-xchacha, and NIST's AES-GCM, and each seals the bytes Python's `cryptography` does, in `ChaCha20Poly1305` and `AESGCM`.

```calx
import std.crypto.{XChaCha20Poly1305, random_bytes}

fn main() {
    // The key is the program's own, kept in std.keyring, say, and never in the database.
    let cipher = XChaCha20Poly1305.new(XChaCha20Poly1305.generate_key())

    // An API token, sealed for the row that stores it, with a new random nonce kept beside it.
    let nonce = random_bytes(24)
    let sealed = cipher.encrypt(nonce, "sk-live-1234".to_bytes(), "mcp_servers/7".to_bytes())
    println("{sealed.len()}")    // out: 28

    let token = cipher.decrypt(nonce, sealed, "mcp_servers/7".to_bytes())!
    println(String.from_utf8(token) ?? "bad")    // out: sk-live-1234

    // Copied to another row, or with a byte changed, it doesn't open.
    println("{cipher.decrypt(nonce, sealed, "mcp_servers/8".to_bytes()) == none}")    // out: true
    var changed = sealed.copy()
    changed[0] ^= 1
    println("{cipher.decrypt(nonce, changed, "mcp_servers/7".to_bytes()) == none}")    // out: true
}
```

```calx
import std.bytes.{from_hex, hex}
import std.crypto.{AesGcm, ChaCha20Poly1305}

fn main() {
    // RFC 8439's test of the AEAD, section 2.8.2: its key, nonce, associated data and tag.
    let c = ChaCha20Poly1305.new(from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")!)
    let message = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.".to_bytes()
    let sealed = c.encrypt(from_hex("070000004041424344454647")!, message, from_hex("50515253c0c1c2c3c4c5c6c7")!)
    println("{sealed.len()} {hex(sealed)[228..]}")    // out: 130 1ae10b594f09e26a7e902ecbd0600691

    // The GCM spec's first test: nothing sealed under a zero key and nonce is just the tag.
    let g = AesGcm.new([0; 16])
    println(hex(g.encrypt([0; 12], [])))    // out: 58e2fccefa7e3061367f1d57a4e7455a
}
```

| Item | Gives |
| --- | --- |
| `ChaCha20Poly1305.new(key: List<u8>) -> ChaCha20Poly1305` | The cipher with a 32-byte key, as `ChaCha20Poly1305(key)`. Panics for another length |
| `ChaCha20Poly1305.generate_key() -> List<u8>` | 32 bytes from the system's random source, as `ChaCha20Poly1305.generate_key()` |
| `c.encrypt(nonce: List<u8>, plaintext: List<u8>, aad: List<u8> = []) -> List<u8>` | The plaintext encrypted, with the 16-byte tag of it and `aad` after it, as `encrypt(nonce, data, aad)`. Panics unless the nonce is 12 bytes, or if the plaintext is over 256 GiB less 64 bytes, as far as ChaCha20's block counter goes |
| `c.decrypt(nonce: List<u8>, sealed: List<u8>, aad: List<u8> = []) -> List<u8>?` | The plaintext, or `none` if the tag doesn't match, where `cryptography` raises `InvalidTag`: a byte of `sealed`, the nonce or `aad` changed, the key is another, or `sealed` is under 16 bytes. `none` too for a nonce that isn't 12 bytes |
| `XChaCha20Poly1305.new(key)`, `XChaCha20Poly1305.generate_key()`, `x.encrypt(nonce, plaintext, aad)`, `x.decrypt(nonce, sealed, aad)` | The same with a 24-byte nonce, as libsodium's `crypto_aead_xchacha20poly1305_ietf` |
| `AesGcm.new(key: List<u8>) -> AesGcm` | The cipher with a key of 16, 24 or 32 bytes, AES-128, AES-192 or AES-256, as `AESGCM(key)`, its round keys and GHASH key worked out once. Panics for another length |
| `AesGcm.generate_key(bits: u32 = 256) -> List<u8>` | A key of 128, 192 or 256 bits from the system's random source, as `AESGCM.generate_key(bit_length)`. Panics for another size |
| `g.encrypt(nonce, plaintext, aad)`, `g.decrypt(nonce, sealed, aad)` | As ChaCha20-Poly1305's, with a nonce of 12 bytes, as NIST advises, or 8 to 128, as `cryptography` takes. The plaintext goes up to 64 GiB less 32 bytes |

- A nonce must never seal two messages under one key. Two messages with the same one give away the xor of their plaintexts, and with AES-GCM what forges any message under the key too. A counter is safe, as TLS, WireGuard and Noise use, one for each direction of a connection. Random nonces are safe with XChaCha20-Poly1305, whose 24 bytes make a repeat as unlikely as guessing the key. With 12 bytes, NIST counts random ones safe for 2^32 messages a key.
- Which to use: XChaCha20-Poly1305 for secrets kept at rest, each with a random nonce stored beside it. ChaCha20-Poly1305 or AES-GCM where a protocol names them: Noise's `ChaChaPoly` is `ChaCha20Poly1305`, and its nonce is 4 zero bytes then the 64-bit counter, little-endian. AES-GCM is the fastest with AES instructions, which nearly every desktop and server processor of the last decade has, and much slower without them, on a Raspberry Pi 4, say, where ChaCha20 is as fast as anywhere.
- The nonce isn't in what's sealed, as it isn't in `cryptography`'s. Store or send it with the sealed bytes.
- `decrypt` compares the tag in a time that doesn't depend on where it differs, and decrypts nothing until it matches. `none` says nothing about why.
- The [tests](../../tests/std/aead.calx) hold RFC 8439's AEAD vectors (2.8.2 and A.5), draft-irtf-cfrg-xchacha-03's (A.3.1), the GCM spec's test cases 1 to 6 and 13 to 16 and 18, and three from NIST's CAVP files, one of them a forgery, each opened and not opened with any bit of it flipped, its nonce, key or associated data changed, or a byte cut off or added. Then what Python's `cryptography` 46 seals for messages of 0 to 70,000 bytes, at the edges of every block size the code works in.
- Checked wider than the tests: all 4,500 of NIST's CAVP cases for GCM with 128-bit tags and IVs of 8 to 128 bytes, for each key size, sealed and opened, the 1,162 marked as forgeries among them refused, and 3,600 random messages, keys and nonces sealed by `cryptography` 46, 1,200 for each cipher. `cryptography` has no XChaCha20-Poly1305, so those were sealed by `cryptography`'s `ChaCha20Poly1305` under HChaCha20's key, worked out from its raw ChaCha20. Without the AES instructions, the code gave the same bytes as with them for 3,000 more random cases of every key size.

## Checked against Java

- AES-CFB8: 300 random streams of up to 699 bytes, encrypted in random pieces, gave the same bytes as Java 25's `Cipher` gave for each whole stream, and decrypting them in other random pieces gave them back. The portable version gave the same as AES-NI on 2000 more.
- RSA: Java decoded `public_der` with `KeyFactory`, encrypted 200 random messages of 0 to 117 bytes with `Cipher`, and `decrypt` opened every one, for 1024 and 2048 bits. Java encoded the key it decoded to the same bytes. Bytes with broken padding gave `none`.
- Signatures: Java 25 made 1024-, 2048- and 4096-bit keys and signed a message with `SHA1withRSA` and `SHA256withRSA`; `verify` takes each, and not with the other hash, another message, or a bit flipped. Java verified what `sign` signed with both, for 1024 and 2048 bits. openssl verified 2048-bit `SHA384withRSA` and `SHA512withRSA` signatures from `sign`, and `verify` took openssl's.
- The [tests](../../tests/std/crypto.calx) hold Java's bytes for one AES stream, and check the rest by round trips and shapes. [Those of signatures](../../tests/std/signatures.calx) hold Java's keys and signatures.

## How it's built

[runtime/calx_crypto.c](../../runtime/calx_crypto.c), linked only into programs that call it, written from FIPS 197 (AES), NIST SP 800-38A (CFB8) and RFC 8017 (RSA and its padding).

| Part | How |
| --- | --- |
| Random bytes | `BCryptGenRandom` on Windows, `getrandom` on Linux |
| AES | AES-NI where the processor has it, which `cpuid` tells when a `Cfb8` is made, or Arm64's AES instructions, which the system tells. Otherwise a byte-at-a-time version, with the S-box worked out from its definition |
| CFB8 encrypting | One AES block for each byte, since each byte's input is the one before it. That's 16 times the work of the block modes, and it's why this mode is slow everywhere |
| CFB8 decrypting | The inputs are the ciphertext, which is all there already, so it runs 16 blocks at once through the AES instructions' pipeline |
| RSA numbers | 64-bit limbs, and Montgomery multiplication with a final subtraction that takes the same time whether it's needed or not |
| RSA private key | Decrypting and signing use the Chinese remainder theorem: two exponents of half the length. Exponents go four bits a step from a table that's read whole each time, so neither the time nor the memory touched depends on the key's bits |
| RSA public key | Read once, with what Montgomery multiplication needs worked out then: R^2 mod n, as 2^k R doubled up from the top power of 2 below n and squared 6 times, so about 64 doublings where 1 doubled 128 times a limb takes 4096 for 2048 bits. The exponent isn't secret, so it goes a bit at a time, a squaring each and a multiply for each that's set: 65537 takes 17 |
| Primes | 300 small primes, then 16 rounds of Miller-Rabin, which a composite passes with odds below 2^-32 |

On our benchmark, an i9-13900K. The decrypt and verify rows are the best of 60 rounds of 2,000, each program on the performance cores, taking turns, with the machine otherwise quiet. The others are one run each:

| | Calx | Java 25 |
| --- | --- | --- |
| CFB8 encrypting | 135 MB/s | 77 MB/s |
| CFB8 decrypting | 659 MB/s | 79 MB/s |
| A 1024-bit key pair | 8.4 ms | 7 to 16 ms |
| A 1024-bit decrypt | 90 µs | 102 µs |
| A 2048-bit `SHA256withRSA` verify, of 32 bytes | 18.5 µs | 21 µs |

The ciphers run for every byte a player sends or gets, so they're what counts. RSA runs once for each login, and a verify once for each signed chat message. Java's best rounds come after its JIT has warmed up. Interleaving the multiply's two carry chains with ADX would make RSA about twice as fast again.

On GitHub's Arm runners, with Arm64's AES instructions, from the [bench workflow](../../.github/workflows/bench.yml) with the nightly of 2026-11-12 (5847f42), rough: CFB8 encrypted at 101 MiB/s and decrypted at 463 MiB/s on a Neoverse N2, Azure's Cobalt 100, on Linux and on Windows alike. A virtual Apple M1 did 69 and 893 MiB/s. In the units of the table above, 101 MiB/s is 106 MB/s.

argon2 is [runtime/calx_argon2.c](../../runtime/calx_argon2.c), written from RFC 9106, and Ed25519 [runtime/calx_ed25519.c](../../runtime/calx_ed25519.c), written from RFC 8032 and the curve's papers. Each is linked only into programs that call it, with [`std.hash`](hash.md)'s runtime for BLAKE2b and SHA-512.

| Part | How |
| --- | --- |
| argon2's blocks | G, BLAKE2b's round with a multiply in each add, over the 1 KiB block as eight rows of 16 words then eight columns, in plain 64-bit C. The block before and the one referenced are xored, permuted and xored back, and from the second pass on the block being overwritten is xored in too, as version 0x13 says |
| argon2's lanes | Each slice of each pass starts a thread for each lane but the first, up to 16 threads with lanes shared out among them, and waits for them at its end: that's where the RFC lets lanes look at each other's blocks. A thread that can't be started has its lanes run on the calling one, which gives the same hash |
| argon2's references | argon2id takes the first half of the first pass's references from a counter run through G, so they don't depend on the password, and the rest from each block before's first word |
| Ed25519's numbers | Mod 2^255 - 19 in five 51-bit limbs, multiplied through 128-bit products with the top's carry folded back in times 19. Inverses and square roots are powers, by RFC 8032's chains of squarings |
| Ed25519's points | Extended coordinates, with the complete sum and doubling of Hisil, Wong, Carter and Dawson's paper: one formula for every pair of points, so nothing branches on them |
| Signing | [a]B four bits at a time from a table of B's first 16 multiples, each entry picked by reading all 16 under masks, and scalars mod L reduced a bit at a time with a subtraction kept or dropped by a mask, so neither the time nor the memory touched depends on the key |
| Verifying | [S]B − [k]A with both tables, four bits at a time, picked directly, since everything in it is public |

On the same desktop, an i9-13900K, the best of several runs, against OpenSSL 3.5 in WSL, the library under Python's `cryptography`:

| | Calx | OpenSSL 3.5 |
| --- | --- | --- |
| argon2id, 64 MiB, 3 passes, 4 lanes | 67 ms, on 4 threads | 320 ms, one thread |
| argon2id, 64 MiB, 3 passes, 1 lane | 145 ms | |
| An Ed25519 signature | 66 µs | 44 µs |
| An Ed25519 verify | 87 µs | 127 µs |

OpenSSL's `kdf` command ran argon2's lanes one after another, as it had no thread pool to run them on. Working G out in vector registers, as the reference implementation's AVX2 code does, would make each lane faster still.

The authenticated ciphers are [runtime/calx_aead.c](../../runtime/calx_aead.c), linked only into programs that call them, written from RFC 8439 (ChaCha20 and Poly1305), draft-irtf-cfrg-xchacha-03 (HChaCha20), FIPS 197 (AES) and NIST SP 800-38D (GCM).

| Part | How |
| --- | --- |
| ChaCha20 | Four blocks at once in vectors of four 32-bit words, a word of every block in each, which clang makes SSE2 or NEON, then turned back into blocks in the registers and xored in 16 bytes at a time. With AVX2, eight blocks at once |
| Poly1305 | Three limbs of 44, 44 and 42 bits, multiplied through 128-bit products, and the last subtraction of 2^130 - 5 kept or dropped by a mask |
| AES | AES-NI, or Arm64's AES instructions, eight blocks at a time in CTR mode, so the AES unit's pipeline stays full. Without them, the S-box is each byte's inverse in GF(2^8), x^254, worked out on bit planes, the same bit of 64 bytes in each word, so no table is read at an address that depends on the key or the data |
| GHASH | PCLMULQDQ, or Arm64's PMULL, eight blocks a step: their eight products with H^8 to H, worked out once for the key, added up and reduced once, as Intel's paper on GCM does it. Without them, 64-bit carry-less products from integer multiplies of every fourth bit, whose carries fall in the gaps between and are masked away, as BearSSL's `ghash_ctmul64` does |
| Opening | The tag is worked out over the ciphertext and compared, every byte whatever the first that differs, before anything is decrypted |
| Keys | Copied into the call and wiped from its stack before it returns, with what's worked out from them |

On the same desktop, an i9-13900K, sealing and opening 16 KiB messages, the best of 7 rounds of 4,000, against OpenSSL 3.5's `speed -evp` in WSL:

| | Calx, sealing | Calx, opening | OpenSSL 3.5 |
| --- | --- | --- | --- |
| ChaCha20-Poly1305 | 1.24 GB/s | 1.34 GB/s | 2.94 GB/s |
| XChaCha20-Poly1305 | 1.42 GB/s | 1.39 GB/s | |
| AES-256-GCM | 4.98 GB/s | 5.14 GB/s | 7.77 GB/s |
| AES-128-GCM | 6.34 GB/s | 6.64 GB/s | 9.34 GB/s |
| AES-256-GCM without AES-NI | 16 MB/s | | |

A 64-byte message seals in 313 ns with ChaCha20-Poly1305 and 91 ns with AES-256-GCM. OpenSSL goes over the message once, encrypting and authenticating each part together, in assembly that works out Poly1305 four blocks at a time in AVX2 too; this goes over it twice, first one and then the other.

`std.crypto` is written in Calx over those runtime calls: [std/crypto.calx](../../std/crypto.calx).
