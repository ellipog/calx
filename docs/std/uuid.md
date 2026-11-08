---
title: std.uuid
description: UUIDs as RFC 9562 defines them and Java's UUID and Python's uuid make them - random, from a name, from Java's nameUUIDFromBytes, read and written as text and bytes.
---

# `std.uuid`

UUIDs, as RFC 9562 defines them and Java's `UUID` and Python's `uuid` make them: at random, from a name, read from text and written back, and as 16 bytes.

```calx
import std.uuid.{Uuid}

fn main() {
    let id = Uuid.random()
    println("{id.version()} {Uuid.parse("{id}") == some(id)}")    // out: 4 true
    println("{Uuid.namespace_dns().v5("python.org")}")             // out: 886313e1-3b8a-5372-9b90-0c9aee199e5d
    let notch = Uuid.from_name("OfflinePlayer:Notch".to_bytes())
    println("{notch} {notch.simple()}")    // out: b50ad385-829d-3141-a216-7e7d7539ba7f b50ad385829d3141a2167e7d7539ba7f
}
```

A `Uuid` is two `u64` halves, `high` and `low`, as Java holds one. It compares as the 128-bit number they make, as Python's does, and can be a map's key.

| Function | Gives |
| --- | --- |
| `Uuid.random()` | A random UUID, version 4, from the system's generator, as Java's `randomUUID` and Python's `uuid4` make it |
| `namespace.v5(name)` | The version 5 UUID of a name in a namespace: the SHA-1 of the namespace's bytes and the name's UTF-8. Python's `uuid5(namespace, name)` |
| `namespace.v3(name)` | The same with MD5, version 3. Python's `uuid3(namespace, name)` |
| `Uuid.namespace_dns()`, `namespace_url()`, `namespace_oid()`, `namespace_x500()` | RFC 9562's namespaces, as Python's `NAMESPACE_DNS` and the others |
| `Uuid.from_name(bytes)` | Java's `UUID.nameUUIDFromBytes`: the MD5 of the bytes, version 3, with no namespace. Minecraft makes an offline player's UUID from `OfflinePlayer:<name>` so |
| `Uuid.parse(text) -> Uuid?` | The UUID written as 32 hex digits in either case, dashed in groups of 8, 4, 4, 4 and 12 or not dashed at all, as Mojang's API writes them; `none` for anything else |
| `{u}` | Lowercase and dashed: `123e4567-e89b-12d3-a456-426614174000`, as Java's `toString` and Python's `str` write it |
| `u.simple()` | The 32 digits without dashes, as Python's `u.hex` writes them |
| `u.to_bytes()`, `Uuid.from_bytes(bytes) -> Uuid?` | The 16 bytes, first to last, as Python's `u.bytes` and `UUID(bytes=...)`; `none` from any other number of bytes |
| `Uuid.new(high, low)`, `u.high`, `u.low` | The halves, as Java's `new UUID(most, least)`, `getMostSignificantBits` and `getLeastSignificantBits` |
| `u.version()` | The four version bits: 4 for random ones, 3 and 5 for named ones |
| `Uuid.nil()` | All zeros |

`Uuid.parse` is strict where Java's `fromString` and Python's `UUID()` aren't: Java takes groups with digits missing, such as `1-2-3-4-5`, and Python takes braces, a `urn:uuid:` prefix and dashes anywhere. Each UUID has one dashed and one plain form here.

## How it's built

[std/uuid.calx](../../std/uuid.calx), in Calx, over [`std.hash`](hash.md) for MD5 and SHA-1 and [`std.crypto`](crypto.md)'s `random_bytes` for random ones. It was checked against Python 3.14's `uuid` on 808 cases: names in each namespace, `nameUUIDFromBytes` as Python works it out, and text in every form, with none different.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs of 12,000 each, against Python 3.14:

| Work | Calx | Python | |
| --- | --- | --- | --- |
| `Uuid.random()` written as text | 1.3 ms | 10.1 ms | 8× |
| `v5` of a 50-byte name | 1.5 ms | 14.0 ms | 9× |
| `Uuid.parse` | 0.22 ms | 7.0 ms | 32× |
