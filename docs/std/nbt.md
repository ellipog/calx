---
title: std.nbt
description: Minecraft's NBT read and written as vanilla 26.3's NbtIo does it, files gzipped, zlib-compressed or plain, tags changed in place, and SNBT as Tag.toString shows it.
---

# `std.nbt`

NBT, the binary format Minecraft keeps its data in: `level.dat`, players' files, structures, and each chunk of a region file. `std.nbt` reads and writes it as vanilla 26.3's `NbtIo` does, and shows it as SNBT as vanilla's `Tag.toString` does. A compound keeps its keys in the order they were read or put, as a Python dict does, so a file read and written back holds the same NBT, byte for byte.

```calx
import std.nbt.{Compression, NbtFile, Tag}

fn main() {
    var data = Tag.Compound(LinkedMap())
    _ = data.insert(String("LevelName"), Tag.String("World"))
    _ = data.insert(String("DayTime"), Tag.Long(6000))
    _ = data.insert(String("SpawnPos"), Tag.List([Tag.Int(0), Tag.Int(64), Tag.Int(0)]))
    var root = Tag.Compound(LinkedMap())
    _ = root.insert(String("Data"), data)

    // A file's bytes, gzipped as vanilla writes level.dat, and read back.
    let bytes = NbtFile.new(root).encode()
    var file = NbtFile.decode(bytes)!
    println("{file.compression == Compression.Gzip} {file.root}")
    // out: true {Data:{DayTime:6000L,LevelName:"World",SpawnPos:[0,64,0]}}

    // Copies of values, for a quick look.
    let d = file.root.get("Data")!
    println("{d.get("DayTime")!.as_int()} {d.get("Weather")} {d.keys()}")
    // out: some(6000) none [LevelName, DayTime, SpawnPos]

    // A match reads the tree where it is, and `= mut` changes it there.
    if let Compound(top) = mut file.root {
        if let Compound(inner) = mut top["Data"] {
            inner.insert(String("DayTime"), Tag.Long(0))
            _ = inner.remove("LevelName")
        }
    }
    println("{file.root}")    // out: {Data:{DayTime:0L,SpawnPos:[0,64,0]}}
}
```

`NbtFile.open(path)` reads a file from disk, and `file.save(path)` writes one, compressed as it was read.

## Files

| Item | Gives |
| --- | --- |
| `NbtFile` | A file: `name`, its root's name, empty in what vanilla writes; `root`, a `Tag.Compound`; and `compression` |
| `NbtFile.new(root: own Tag) -> NbtFile` | A file of `root`, named "" and gzipped, as vanilla's `NbtIo.writeCompressed` writes one |
| `NbtFile.open(path: String) -> NbtFile ! Error` | The file at `path`, gzipped, zlib-compressed or plain, told apart by their first bytes |
| `NbtFile.decode(bytes: List<u8>) -> NbtFile ! NbtError` | The file in `bytes`: gzip if they start `1F 8B`, zlib if they start with a zlib header, plain otherwise. Its root must be a compound, as vanilla's `NbtIo.read` wants, and bytes after it are ignored, as vanilla ignores them |
| `file.encode() -> List<u8>` | The file's bytes, compressed as `compression` says, at level 6, as Java's streams compress by default |
| `file.save(path: String) ! Error` | Writes `file.encode()` to `path` |
| `Compression` | `None`, as `NbtIo.write` writes; `Gzip`, as `level.dat`, players' files and structures are; `Zlib`, as most chunks in a region file are |

## Tags

| Variant | Holds | SNBT |
| --- | --- | --- |
| `End` | Nothing: the byte that ends a compound, and a packet's "no data" | `END` |
| `Byte(i8)`, `Short(i16)`, `Int(i32)`, `Long(i64)` | A number | `1b`, `1s`, `1`, `1L` |
| `Float(f32)`, `Double(f64)` | A number | `1.5f`, `1.5d` |
| `String(String)` | Text, at most 65,535 bytes in Modified UTF-8 | `"a"` |
| `List(List<Tag>)` | Tags, of one kind or several | `[1,2]` |
| `Compound(LinkedMap<String, Tag>)` | Named tags, in the order they were read or put | `{a:1}` |
| `ByteArray(List<i8>)`, `IntArray(List<i32>)`, `LongArray(List<i64>)` | Numbers | `[B;1B]`, `[I;1]`, `[L;1L]` |

A `match` reads a tag where it is, and a [`match mut` or `if let ... = mut`](../reference/patterns.md#changing-the-parts-in-place) changes it there. The methods are for the common cases:

| Method | Gives |
| --- | --- |
| `t.id() -> u8` | The kind's number in the binary format, 0 for `End` up to 12 for `LongArray` |
| `t.kind_name() -> String` | The kind's name as vanilla's `TagType.getName` gives it: `BYTE`, `INT[]`, `COMPOUND` |
| `t.get(key: String) -> Tag?` | A copy of the tag at `key`, if `t` is a compound that has one |
| `t.at(i: usize) -> Tag?` | A copy of element `i`, if `t` is a list that long |
| `t.as_int() -> i64?` | The number, if `t` is a byte, a short, an int or a long |
| `t.as_float() -> f64?` | The number, if `t` is one of any kind, as the nearest `f64` |
| `t.as_bool() -> bool?` | Whether the number isn't 0, if `t` is a byte, a short, an int or a long, as NBT keeps a boolean |
| `t.as_str() -> String?` | The text, if `t` is a string |
| `t.len() -> usize` | How many tags a list or a compound holds, or numbers an array does; 0 for anything else |
| `t.keys() -> List<String>` | A compound's keys, in order; none for anything else |
| `t.insert(key: own String, value: own Tag) -> bool` | Puts `value` at `key` if `t` is a compound, and says whether it is. A key that's there keeps its place |
| `t.remove(key: String) -> Tag?` | Takes the tag at `key` out, if `t` is a compound that has one |
| `t.push(value: own Tag) -> bool` | Adds `value` at the end if `t` is a list, and says whether it is |
| `t.to_snbt() -> String`, `"{t}"` | The tag as SNBT, as vanilla's `Tag.toString` writes it |

## Tags without a file

Packets send a tag with its kind and no name, and a file holds its root with a name. These read and write either from where it sits in a buffer:

```calx
import std.nbt.{Tag, decode_any, encode_any}
import std.bytes.{hex}

fn main() {
    var out = List<u8>()
    encode_any(Tag.List([Tag.Int(1), Tag.String("a")]), mut out)
    println(hex(out))    // out: 090a00000002030000000000010008000000016100
    var at: usize = 0
    let back = decode_any(out, mut at)!
    println("{back} {at}")    // out: [1,"a"] 21
}
```

| Item | Gives |
| --- | --- |
| `decode_any(bytes: List<u8>, at: mut usize) -> Tag ! NbtError` | The tag at `at`, as `NbtIo.readAnyTag` reads it, and moves `at` past it. A lone 0 is `Tag.End` |
| `decode_named(bytes: List<u8>, at: mut usize) -> (String, Tag) ! NbtError` | The named tag at `at`, as `NbtIo.read` reads a file's root, and moves `at` past it |
| `encode_any(tag: Tag, out: mut List<u8>)` | Adds `tag` to `out` as `NbtIo.writeAnyTag` writes it |
| `encode_named(name: String, tag: Tag, out: mut List<u8>)` | Adds `tag` to `out` named `name`, as a file holds its root |

Reading fails with an `NbtError`, which says where, in bytes, as vanilla says what's wrong:

| `NbtError` | When |
| --- | --- |
| `Truncated` | The bytes end before the tag does |
| `BadKind { kind, at }` | The kind byte at `at` isn't one of NBT's 13 |
| `BadString(at)` | The string whose length is at `at` isn't Modified UTF-8 |
| `NegativeLength { length, at }` | A list's or an array's length is below 0 |
| `ListWithoutKind(at)` | A list has elements but no kind for them, vanilla's "Missing type on ListTag" |
| `TooDeep(at)` | Lists and compounds nest more than 512 deep, vanilla's limit |
| `NotCompound` | A file's root isn't a compound |
| `Compressed(e)` | The gzip or zlib stream around the NBT is broken, with [`std.zlib`](zlib.md)'s error |

Writing panics on what NBT can't hold: a string past 65,535 bytes, which Java's `writeUTF` can't write either, and an `End` inside a list or a compound.

## As vanilla does it

`std.nbt` was checked against vanilla 26.3's own `NbtIo` and `Tag.toString`, run as a black box:

- On 80,000 random byte strings, most of them NBT, it accepted and rejected the same ones, with the same SNBT and the same bytes left over. What vanilla wrote back of each, `std.nbt` read and wrote again byte for byte.
- On all 1,975 NBT files of vanilla's structures and of worlds vanilla saved, it gave the same SNBT, and wrote back the NBT it read byte for byte.

What that covers:

- SNBT sorts a compound's keys as Java sorts strings, by UTF-16 units. A key is bare if it starts with a letter, `.` or `_`, goes on with letters, digits and `_ - . +`, and isn't `true` or `false` in any case. Otherwise it's quoted, and so is a string.
- A quoted key or string takes `"`, unless it holds a `"` before any `'`; then it takes `'`. `\b \t \n \f \r` are escaped so, and other control characters as `\x1B`.
- A list of several kinds is written as compounds that each hold one tag under the empty key, as vanilla has done since 1.21.5, and it's read back unwrapped. An empty list is written with kind 0.
- A float or a double that's NaN is written as Java's one NaN, `7FC00000` or `7FF8000000000000`. -0.0 reads as 0.0, as vanilla's `FloatTag.valueOf` and `DoubleTag.valueOf` give their zero for it.
- A key given twice keeps its first place and takes its last value, as Java's map does.
- Lists and compounds nest at most 512 deep. An empty list of a kind past 12 is accepted, since vanilla doesn't look at its kind.

It differs from vanilla in three places:

- A compound is written in its keys' order, where vanilla writes its hash map's order. For a file vanilla wrote, the two are the same.
- A string holding half a surrogate pair reads as `?` in its place, as [`std.text.mutf8`](text-mutf8.md) decodes it, since a Calx `String` is UTF-8 and can't hold one. Java keeps it.
- Gzip and zlib come from [`std.zlib`](zlib.md), which picks its own matches, so a compressed file isn't Java's bytes, though the NBT inside is. Either reads the other's.

Compound keys are [interned](../reference/strings.md#interned-strings): each is kept once, so a tree of many compounds with the same keys holds one copy of each, and reading one costs no allocation. The runtime keeps a bounded number of names, and a key past that limit is an ordinary copy.

## Speed

Times for 1,511 structure files from vanilla 26.3, 45 MB of NBT, on one P-core of an i9-13900K. Each time is the best of 5 passes over all the files, after 5 to warm up for Java.

| | std.nbt | Vanilla's `NbtIo`, Java 25 | nbtlib 2.0.4, Python 3.14 |
| --- | --- | --- | --- |
| Open: gunzip and read | 141 ms | 151 ms | 7,772 ms |
| Read the NBT already gunzipped | 114 ms | 117 ms | 7,594 ms |
| Write it | 75 ms | 248 ms | 1,547 ms |
| Show it as SNBT | 80 ms | 156 ms | 5,129 ms |

On this benchmark std.nbt reads a little faster than vanilla, writes 3 times as fast, and is between 20 and 66 times as fast as nbtlib. Each tag takes 64 bytes, the size of its biggest kind, a compound's map, so a list of numbers takes more memory than an array of them does.

`std.nbt` is written in Calx: [std/nbt.calx](../../std/nbt.calx).
