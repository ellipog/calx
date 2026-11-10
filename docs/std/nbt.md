---
title: std.nbt
description: Minecraft's NBT read and written as vanilla 26.3's NbtIo does it, files gzipped, zlib-compressed or plain, tags changed in place, SNBT read, shown and laid out, and NBT paths, as vanilla does.
---

# `std.nbt`

NBT, the binary format Minecraft keeps its data in: `level.dat`, players' files, structures, and each chunk of a region file. `std.nbt` reads and writes it as vanilla 26.3's `NbtIo` does, and reads and writes [SNBT](#snbt), its text form, as vanilla's `TagParser`, `Tag.toString` and pretty printer do. A compound keeps its keys in the order they were read or put, as a Python dict does, so a file read and written back holds the same NBT, byte for byte.

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
| `t.to_pretty_snbt() -> String` | The tag as SNBT laid out to be read, as vanilla's `SnbtPrinterTagVisitor` writes it ([SNBT](#snbt)) |

## SNBT

SNBT is NBT as text, as commands, datapacks and structures' `.snbt` files hold it. `parse_snbt` reads it as vanilla 26.3's `TagParser` does, and a tag is written back compact or laid out:

```calx
import std.nbt.{parse_snbt}

fn main() {
    let t = parse_snbt("\{Count:1b,id:\"minecraft:stone\",Pos:[1.5d,64.0d,-3.0d],flags:[B;1,0x7f],on:true\}")!
    println("{t}")
    // out: {Count:1b,Pos:[1.5d,64.0d,-3.0d],flags:[B;1B,127B],id:"minecraft:stone",on:1b}
    println(t.to_pretty_snbt())
    // out: {
    // out:     Count: 1b,
    // out:     Pos: [
    // out:         1.5d,
    // out:         64.0d,
    // out:         -3.0d
    // out:     ],
    // out:     flags: [B; 1B, 127B],
    // out:     id: "minecraft:stone",
    // out:     on: 1b
    // out: }
    match parse_snbt("[1, 2") {
        ok(_) => println("read"),
        err(e) => println("{e}"),    // out: line 1, column 6: the end in a list, where `,` or `]` should be
    }
}
```

| Item | Gives |
| --- | --- |
| `parse_snbt(text: String) -> Tag ! SnbtError` | The tag in `text`, with white space around it allowed |
| `t.to_snbt()`, `"{t}"` | The tag compact, as vanilla's `Tag.toString` writes it, with a compound's keys in Java's order |
| `t.to_pretty_snbt()` | The tag laid out: four spaces a level and `key: value`. A structure's keys come in the order vanilla gives them, and its size, blocks, palette entries and entities go on one line each, as in vanilla's `.snbt` files |
| `SnbtError` | Why the text isn't SNBT: `message`, and the `line` and `column` where it went wrong, both from 1 |

What `parse_snbt` takes, as vanilla does:

- Integers: `1` is an int, and `1b`, `1s`, `1i` and `1L` the other kinds, in either case. `u` or `s` before the letter makes it unsigned or signed, so `255ub` is -1b. Hex `0xFF` and binary `0b101` are unsigned unless they have `s`, so `0xFFFFFFFF` is -1, and a negative one needs it, as `-0x80sb`. `_` may go between digits, as `1_000`. A decimal integer can't start with 0, unless it's 0.
- Floats and doubles: `1.5f` and `1.5d`, or `1.5`, `.5`, `1.` and `1e3` for doubles, rounded once from the text as Java does. One too big for its kind is an error, and -0.0 reads as 0.0.
- White space between a number's parts, such as `1 .5` or `0x1F b`, though not inside its digits.
- Strings in `"` or `'`, with the escapes `\\ \' \" \b \s \t \n \f \r`, `\xHH`, `\uHHHH` and `\UHHHHHHHH`, and white space allowed between the `\` and its letter. A bare word of letters, digits and `_ - . +` that starts with a letter or `_` is a string too, and `true` and `false` in any case are 1b and 0b.
- `bool(x)`, which is 1b if the number `x` isn't 0, and `uuid("...")`, the UUID's four ints, as Java's `UUID.fromString` reads it.
- Arrays, `[B; 1, 2]`, whose integers take the array's kind unless they have a narrower one, lists of any kinds, and compounds, whose keys are quoted and not empty, or bare, and both may end with a comma.
- White space anywhere between parts: what Java's `Character.isWhitespace` says, which isn't true of no-break spaces.

## NBT paths

An `NbtPath` names tags inside a tag, as vanilla's `/data` and `/execute store` commands take one, and finds, sets, removes and inserts them as vanilla 26.3's `NbtPathArgument` does:

```calx
import std.nbt.{NbtPath, Tag, parse_snbt}

fn main() {
    var player = parse_snbt("\{Inventory:[\{Slot:0b,id:\"minecraft:stone\",count:64\},\{Slot:1b,id:\"minecraft:dirt\",count:3\}],Pos:[0.5d,64.0d,0.5d]\}")!
    println("{NbtPath.parse("Inventory[].id")!.get(player)!}")    // out: ["minecraft:stone", "minecraft:dirt"]
    println("{NbtPath.parse("Pos[-2]")!.get(player)!}")    // out: [64.0d]

    // Set the count of the item in slot 1, and add a torch at the end.
    let count = NbtPath.parse("Inventory[\{Slot:1b\}].count")!
    println("{count.set(mut player, Tag.Int(10))!}")    // out: 1
    let torch = parse_snbt("\{Slot:2b,id:\"minecraft:torch\",count:1\}")!
    _ = NbtPath.parse("Inventory")!.insert(mut player, -1, [torch])!
    println("{NbtPath.parse("Inventory[]")!.count(player)}")    // out: 3

    // What isn't there is made on the way.
    _ = NbtPath.parse("Tags.home.x")!.set(mut player, Tag.Int(5))!
    _ = NbtPath.parse("Inventory[\{id:\"minecraft:stone\"\}]")!.remove(mut player)!
    println("{player}")
    // out: {Inventory:[{Slot:1b,count:10,id:"minecraft:dirt"},{Slot:2b,count:1,id:"minecraft:torch"}],Pos:[0.5d,64.0d,0.5d],Tags:{home:{x:5}}}
    match NbtPath.parse("Armor[0]")!.get(player) {
        ok(_) => println("found"),
        err(e) => println("{e}"),    // out: nothing matches `Armor`
    }
}
```

A path's parts, joined by `.` before a key:

| Part | Names |
| --- | --- |
| `name`, `"a name"` | The tag at a compound's key. A bare key is anything up to a space, a quote, a bracket, a brace or `.`, and a quoted one takes `\\` and `\"` |
| `name{...}` | The compound at the key, if it holds what the SNBT compound does |
| `[i]` | A list's or an array's element, counted from the end if `i` is below 0, so `[-1]` is the last |
| `[]` | Every element of a list or an array |
| `[{...}]` | Each compound in a list that holds what the SNBT compound does |
| `{...}`, first | The root, if it holds what the compound does |

A compound holds what a pattern does if it has each of the pattern's keys with a value that matches. A list matches if it holds, for each of the pattern's elements, one that matches, in any order, and an empty list only matches an empty one. Anything else must be equal: `{a:1}` doesn't match `{a:1b}`.

| Item | Gives |
| --- | --- |
| `NbtPath.parse(text) -> NbtPath ! SnbtError` | The path in `text`, all of it. An empty one names the root |
| `p.get(root) -> List<Tag> ! PathError` | Copies of the tags the path names, in order, or `NoMatch` |
| `p.count(root) -> usize` | How many tags the path names |
| `p.set(root: mut Tag, value) -> usize ! PathError` | Puts a copy of `value` at each place the path names, as `/data modify ... set` does, and gives how many tags changed. One already equal to `value` isn't counted |
| `p.remove(root: mut Tag) -> usize ! PathError` | Takes out what the path names, as `/data remove` does, and gives how many tags it took |
| `p.insert(root: mut Tag, index: i32, items: List<Tag>) -> usize ! PathError` | Puts copies of `items` into each list or array the path names, from `index`: 0 is before the first, as `prepend` puts them, and -1 after the last, as `append` does. Gives how many lists took an item |
| `p.get_or_create(root: mut Tag, value) -> List<Tag> ! PathError` | Copies of the tags the path names, after making what's missing, with a copy of `value` at the end |
| `"{p}"` | The path as it was written |

Changing through a path makes what's missing on the way, as vanilla does: a compound where a key comes next and a list where `[...]` does, the pattern's compound for `name{...}` or `[{...}]`, and one element in an empty list for `[]`. An array takes a number as its kind does in vanilla: an integer's low bits, a float or a double rounded down and clamped to an int, and for a long array, a float cut toward 0 and a double rounded down. Anything else isn't put in.

| `PathError` | When |
| --- | --- |
| `NoMatch(path)` | The path names nothing, or a part before the last found nothing to change in. `path` is the path as far as that part |
| `NotList(tag)` | Inserting found `tag` where a list or an array should be |
| `BadIndex { index, len }` | Inserting at an index outside a list of `len`, which takes from `-len - 1` to `len` |
| `TooDeep` | The path's parts and the value's depth come to more than 512, which vanilla refuses before changing anything |
| `Empty` | Setting, removing or inserting through an empty path, which names no tag in the root to change |

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

SNBT was checked against vanilla 26.3's `TagParser` and `SnbtPrinterTagVisitor` the same way:

- On 855 texts written to probe each rule above, and 45,000 random ones, `parse_snbt` accepted and rejected the same ones, read the same tag, kind for kind, and wrote it compact and laid out as vanilla does.
- On 160,000 random decimals, floats and doubles of up to 20 digits with exponents up to 40, it rounded each to the same value.
- All 1,975 files, written compact and laid out, read back as the tags they were written from.

`parse_snbt` differs from vanilla in four places:

- `\N{name}`, a character by its Unicode name, is an error for now, where vanilla looks the name up.
- Half a surrogate pair, such as `\uD800` on its own, reads as `?`, as a Calx `String` can't hold it.
- Lists and compounds nest at most 512 deep, as in NBT. Vanilla's parser has no limit and slows down a lot on deep text.
- Its errors are its own, a line and a column and what was found where, though it fails on the same text.

NBT paths were checked against vanilla's `NbtPathArgument` on 748 cases written to probe each rule, and 140,000 random ones over small trees, with every kind of part and change. `std.nbt` read the same paths, found the same tags, changed the same ones to the same values with the same counts, and failed on the same changes with the same part named. Where a change fails partway, as an insert into the second of two lists does, it left the tag as vanilla left it. It differs in four places:

- `NbtPath.parse` reads the whole text, where a command stops at a space and leaves the rest for what comes after.
- An empty path can't be changed through, and `a[` is an error. Vanilla's code fails on both with a Java exception.
- A value nested past 512 can't be read from SNBT, as above, so it can't be given to a change from text.
- Its messages are its own.

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

SNBT, on the same files as SNBT, 30 MB of it written compact, and timed the same way:

| | std.nbt | Vanilla's `TagParser` and `SnbtPrinterTagVisitor`, Java 25 | nbtlib 2.0.4, Python 3.14 |
| --- | --- | --- | --- |
| Read it compact | 219 ms | 3,310 ms | 22,245 ms |
| Read it laid out | 387 ms | 5,154 ms | |
| Lay it out | 106 ms | 891 ms | 6,154 ms |

nbtlib's times are for its own SNBT: `parse_nbt` on what `serialize_tag(compact=True)` writes, and `serialize_tag(indent=4)`. On this benchmark std.nbt reads SNBT 13 to 15 times as fast as vanilla and 100 times as fast as nbtlib, and lays it out 8 times as fast as vanilla and 58 times as fast as nbtlib.

`std.nbt` is written in Calx: [std/nbt.calx](../../std/nbt.calx).
