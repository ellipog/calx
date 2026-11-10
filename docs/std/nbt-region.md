---
title: std.nbt.region
description: Minecraft's region files, the .mca files a world keeps its chunks in, read and written as vanilla 26.3's RegionFile reads and writes them - chunks in each compression, their own .mcc files, and broken chunks.
---

# `std.nbt.region`

Region files, the `r.X.Z.mca` files a Minecraft world keeps its chunks in, read and written as vanilla 26.3's `RegionFile` reads and writes them. A region file holds the chunks of 32 × 32 columns. Each chunk sits in whole sectors of 4 KiB, after a header that says where each one is and when it was saved. A chunk that needs 256 sectors or more goes in a file of its own beside it, `c.X.Z.mcc`. A chunk is NBT, read and written with [std.nbt](nbt.md).

```calx
import std.fs.{temp_dir, remove_dir_all}
import std.nbt.{parse_snbt}
import std.nbt.region.{Compression, RegionFile, region_file_name, region_of}

fn main() {
    let dir = temp_dir()!
    // The chunk at x 40, z -7 is in region 1, -1.
    let (rx, rz) = region_of(40, -7)
    let path = "{dir}/{region_file_name(rx, rz)}"
    println(region_file_name(rx, rz))    // out: r.1.-1.mca

    // A file that isn't there is made, as vanilla makes it.
    var region = RegionFile.open(path)!
    region.write(40, -7, parse_snbt("\{xPos: 40, zPos: -7, Status: \"minecraft:full\"\}")!)!
    region.compression = Compression.Lz4
    region.write(41, -7, parse_snbt("\{xPos: 41, zPos: -7\}")!)!
    region.close()!

    // Read only, so the file stays as it is.
    var world = RegionFile.open_read(path)!
    for (x, z) in world.chunks() {
        println("{x} {z} {world.read(x, z)!!}")
    }
    // out: 40 -7 {Status:"minecraft:full",xPos:40,zPos:-7}
    // out: 41 -7 {xPos:41,zPos:-7}
    println("{world.has(42, -7)} {world.read(42, -7)! == none}")    // out: false true
    world.close()!
    remove_dir_all(dir)!
}
```

A chunk is named by its coordinates in the world, as vanilla's `ChunkPos` names it. Only their last five bits pick its place in the file, but a chunk in a file of its own is named by all of them, so pass the world's coordinates.

| Item | Gives |
| --- | --- |
| `RegionFile.open(path: String) -> RegionFile ! FsError` | The region file at `path`, kept open until `close`, as vanilla's constructor opens it: a file that isn't there is made, empty, and each place in the header that can't be right is dropped |
| `RegionFile.open_read(path: String) -> RegionFile ! FsError` | The same, opened only to read: `NotFound` if nothing's there, `write` and `clear` fail with `Denied`, and `close` leaves the file as it was. For reading a world without changing it |
| `region.compression` | How `write` compresses chunks: `Compression.Zlib` unless it's set, vanilla's default |
| `region.sync` | Whether `write` and `clear` return only once what they wrote is on the disk, as vanilla's `sync-chunk-writes`. Off unless it's set |
| `region.chunks() -> List<(i32, i32)>` | The coordinates of each chunk the header has a place for, x first, from the region's coordinates in its file's name. Another name counts as region 0, 0 |
| `region.has(x: i32, z: i32) -> bool` | Whether the header has a place for the chunk, as `hasChunk` says. The chunk may still be broken |
| `region.exists(x: i32, z: i32) -> bool` | Whether the chunk looks whole, as `doesChunkExist` says: its first 5 bytes are in the file, its compression is one vanilla knows, and its length fits its sectors, or its own file is there |
| `region.timestamp(x: i32, z: i32) -> i32` | When the chunk was last written or cleared, in seconds since 1970, or 0. Vanilla writes it as a 32-bit int |
| `region.read(x: i32, z: i32) -> Tag? ! RegionError` | The chunk's root compound, or `none` if the header has no place for it |
| `region.read_bytes(x: i32, z: i32) -> List<u8>? ! RegionError` | The chunk's bytes, uncompressed, as `getChunkDataInputStream` gives them, or `none` |
| `region.write(x: i32, z: i32, root: Tag) ! FsError` | Writes the compound as the chunk, as vanilla's `NbtIo.write` writes it, compressed as `compression` says, with the time now |
| `region.write_bytes(x: i32, z: i32, bytes: List<u8>) ! FsError` | Writes the bytes as the chunk, compressed as `compression` says, with the time now |
| `region.clear(x: i32, z: i32) ! FsError` | Takes the chunk out, as vanilla's `clear` does: its place goes, its time is now, and its own file is removed |
| `region.close() ! FsError` | Pads the file to whole sectors, puts it on the disk and closes it, as vanilla does. Reading after it opens the file again for each read |
| `Compression` | `Gzip` (1), `Zlib` (2, `deflate`), `None` (3, `none`) or `Lz4` (4, `lz4`), vanilla's `RegionFileVersion`. `id()` gives the byte, `from_id(id)` and `from_name(name)` the compression, `from_name` by the names `region-file-compression` takes |
| `region_of(x: i32, z: i32) -> (i32, i32)` | The coordinates of the region that holds the chunk: each divided by 32, rounded down |
| `region_file_name(x: i32, z: i32) -> String` | The name of the region's file, `r.X.Z.mca` |

## Writing

Each write goes where vanilla's would, so a file written the same way holds the same bytes:

- A chunk is its length plus one as 4 bytes, its compression's byte, then its bytes compressed, in as many sectors as that needs.
- It goes in the first free run of sectors that fits it, or after the last taken sector. Its old sectors are freed only after the new ones are taken, so a chunk written again moves, and the next chunk can fill the gap it left.
- The bytes of a chunk are written as they are, so a sector it doesn't fill keeps what was there before, as vanilla's do. The file is padded to whole sectors with zeros.
- The header, both tables of 4 KiB, is written whole after each write and clear, with the chunk's time.
- A chunk that needs 256 sectors or more goes in `c.X.Z.mcc` beside the file, and the region keeps a stub of one sector that says so. A chunk written back inside the file has its `.mcc` removed.

The chunk's new sectors are written before the header that points at them, so a program stopped part way leaves the old chunk where the header on the disk says it is. With `sync`, each step is on the disk before the next. A chunk going to a `.mcc` is written beside it first and moved into place before the header is written, so the `.mcc` is always the old chunk or the new one, whole. Vanilla moves it after the header, which leaves the chunk missing if it's stopped in between. Either way, the files come out the same.

Gzip and zlib compress at level 6, as Java's streams do, but with [std.zlib](zlib.md)'s own matches, so their bytes differ from vanilla's though each reads the other's. Chunks written with `None` or `Lz4` come out byte for byte as vanilla writes them.

## Reading

`read` and `read_bytes` give `none` when the header has no place for the chunk. For a chunk that's there but broken, they fail with a `RegionError`. Vanilla reads the first six as no chunk, logging why, so the game makes the chunk again; `vanilla_missing()` says which those are. It throws for the rest.

| Error | When | Vanilla |
| --- | --- | --- |
| `Truncated` | The file ends before the chunk's first 5 bytes, or before as many bytes as its length says | missing |
| `Empty` | Its length is 0, "allocated, but stream is missing" | missing |
| `NegativeLength(n)` | Its length is below 0 | missing |
| `BadCompression(id)` | Its compression byte isn't 1 to 4 or 127 | missing |
| `NoExternal(path)` | It's in a file of its own that isn't there, or isn't a file | missing |
| `Custom(name)` | It's compressed by a mod's compression, 127, which vanilla can't read | missing |
| `BadCustom` | The name of its mod's compression is cut short or isn't Modified UTF-8 | throws |
| `Zlib(e)` | Its gzip or zlib stream is broken, as [std.zlib](zlib.md) says | throws |
| `Lz4(e)` | Its LZ4 stream is broken, as [std.lz4](lz4.md) says | throws |
| `Nbt(e)` | Its NBT is broken, or isn't a compound. Only `read` gives this | throws |
| `Fs(e)` | The file couldn't be read | throws |

As the file opens, a place in the header is dropped if it starts in the header's own two sectors, takes no sectors, or starts past the end of the file. A header cut short reads as zeros where it's missing. A chunk in a file of its own has its length in the region ignored, but for 0.

## Checked against vanilla

The rig ran vanilla 26.3's `RegionFile` and `std.nbt.region` on the same files and the same steps, and compared what each read, what `has` and `exists` said, and the files each left, byte for byte, with the times written masked:

- 97 cases of single steps: new files, sector edges, chunks written again, cleared, of their own files and back, headers cut short or wrong, and chunks of every way to be broken. All matched, but for a chunk of 2 MB of zeros compressed by zlib, whose bytes differ as above.
- 134 more: 100 random runs of writes, clears and reads of up to 260 sectors, 40 of them on files with random headers, some places broken and overlapping; mods' compressions; zlib's dictionaries, two streams and bytes after the end; empty streams of each kind; and writes in every compression read back. All matched, but for gzip and zlib of 70,000 zeros, as above.
- Every chunk of 1,562 region files vanilla saved, 1.9 GB: the same bytes for each of 240,136 chunks.

## Speed

Every chunk of 52 region files of a world vanilla made, 188 MB, 928 MB uncompressed, read the way each would read them: `std.nbt.region`, vanilla's `RegionFile` on Java 25, and a Python 3.14 script that reads each file whole and inflates each chunk with `zlib`. Best of 9 passes, each after 2 to warm up, pinned to the P-cores of an i9-13900K:

| | Time |
| --- | --- |
| Calx | 659 ms |
| Python | 794 ms |
| Java | 1,748 ms |

Nearly all of it is inflating zlib. Python's time is mostly its `zlib` module, which is C.
