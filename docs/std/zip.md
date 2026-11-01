---
title: std.zip
description: Zip archives read and written in memory - stored and deflated entries, Zip64, Unix file modes, and extracting to a folder without letting a path climb out of it.
---

# `std.zip`

Zip archives, read from memory as Java's `ZipFile` reads them, and written as `zip` writes them. Calcite reads Mojang's server jar with it, and Calx's updater reads its Windows releases.

```calx
import std.zip.{Writer, Zip}

fn main() {
    var w = Writer.new()
    w.add("notes/hello.txt", "hello hello hello hello".to_bytes())
    w.add_with("bin/run", "#!/bin/sh".to_bytes(), 9, 0o755)
    w.add_dir("empty")
    let bytes = w.finish()
    println("{bytes.len()} bytes")    // out: 324 bytes

    let z = Zip.open(bytes)!
    for e in z.entries() {
        println("{e.name} {e.size} {e.mode == 0o755} {e.is_dir()}")
    }
    // out: notes/hello.txt 23 false false
    // out: bin/run 9 true false
    // out: empty/ 0 true true
    let text = z.read_named("notes/hello.txt")!
    println(String.from_utf8(text) ?? "not UTF-8")    // out: hello hello hello hello
    match z.read_named("missing") {
        ok(_) => println("found it"),
        err(e) => println(e),    // out: no entry missing
    }
}
```

To work with a file, read it with [`std.fs.read`](fs.md) and write the archive with `std.fs.write`.

## Reading

| Item | Gives |
| --- | --- |
| `Zip.open(bytes: own List<u8>) -> Zip ! String` | The archive, read from its central directory, or why it can't be. The archive keeps the bytes |
| `Zip.walk(bytes: own List<u8>) -> Zip ! String` | The files read one after another from the start, for a stretch of an archive without its central directory, as Java's `ZipInputStream` reads |
| `z.entries() -> List<Entry>` | Its files and directories, in the central directory's order |
| `z.len() -> usize`, `z.name(i: usize) -> String` | How many there are, and the path of one |
| `z.find(name: String) -> usize?` | The index of the entry at `name`. A later entry of the same name wins |
| `z.read(i: usize) -> List<u8> ! String` | What entry `i` holds, inflated and checked against its CRC-32 |
| `z.read_named(name: String) -> List<u8> ! String` | What the entry at `name` holds |
| `z.extract(to: String) ! String` | Writes every entry into the folder `to`, making folders on the way, and gives each file its mode where the archive has one |

`Entry` has `name: String`, which ends with `/` for a directory; `size: usize`; `mode: u32`, the permission bits when it was archived on Unix, or 0; and `modified: DateTime` ([std.time](time.md)), local to whoever archived it, to two seconds. `e.is_dir()` says whether it's a directory.

- **Errors** are Java's `ZipException` messages, such as `invalid CEN header (bad signature)`, so a server can log what vanilla logs.
- **Methods:** stored (0) and deflated (8). Anything else, and encrypted entries, are errors.
- **Zip64** is read wherever an archive uses it: sizes and offsets past 4 GiB, and more than 65,535 entries.
- **Extracting is safe from hostile archives.** Before writing anything, `extract` checks every path and fails if one is absolute, names a drive, or has a `..` that climbs out of `to`. Backslashes count as separators, since some Windows tools wrote them.

## Writing

| Item | Does |
| --- | --- |
| `Writer.new() -> Writer` | An empty archive |
| `w.add(name: String, bytes: List<u8>)` | Adds a file, deflated at level 6, with mode `0o644` |
| `w.add_with(name: String, bytes: List<u8>, level: u32, mode: u32)` | Adds a file deflated at `level`, from 1, fastest, to 9, smallest, or stored with 0, with the permission bits `mode` |
| `w.add_dir(name: String)` | Adds a directory, with mode `0o755`. The name gets its `/` if it hasn't got it |
| `w.set_time(at: DateTime)` | Dates the entries added after it, to two seconds, between 1980 and 2107 |
| `w.finish() -> List<u8>` | The archive, with its central directory |

- **The same files make the same bytes.** Entries are dated 1980-01-01 00:00 until `set_time` says otherwise, so an archive built twice is identical, which keeps hashes and caches stable.
- **A file deflate can't shrink is stored.**
- **Modes survive.** Each entry is marked as made on Unix, with its mode in the external attributes, so `unzip` and Python's `zipfile` restore `0o755`.
- **Names are UTF-8**, and say so with the flag that `unzip`, Windows' Explorer and bsdtar honor.
- **Zip64** is written only where it's needed: a file of 4 GiB or more, an offset past 4 GiB, or more than 65,535 entries.

`std.zip` is written in Calx over [std.zlib](zlib.md): [std/zip.calx](../../std/zip.calx). Python's `zipfile` and bsdtar read what it writes, Zip64 included.
