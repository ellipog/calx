---
title: std.palette
description: Minecraft's paletted containers - a chunk section's block states and biomes as palettes and bits packed in longs, grown, sent, saved and loaded as Minecraft 26.3 does - BitStorage, Strategy, PalettedContainer.
---

# `std.palette`

A chunk section holds 4,096 block states and 64 biomes. Minecraft keeps each set in a `PalettedContainer`: a short palette of the values that are there, and an index into it for each position, packed into longs by a `SimpleBitStorage`. The palette changes kind as it grows, and the client and region files get it as it is, so the bytes depend on which entries a palette has and in what order.

`std.palette` grows, sends, saves and loads containers as Minecraft 26.3 does. The bytes a client is sent and the palettes and longs a region file holds come out the same.

```calx
import std.palette.{Packed, PalettedContainer, Strategy}

fn main() {
    // Bedrock, dirt, dirt and grass, over air, in a registry of 35,723 block states.
    let blocks = Strategy.block_states(35723)
    var section = PalettedContainer.new(0, blocks)
    for x in 0..16 {
        for z in 0..16 {
            section.set(x, 0, z, 88)
            section.set(x, 1, z, 10)
            section.set(x, 2, z, 10)
            section.set(x, 3, z, 9)
        }
    }
    println("{section.bits()} {section.get(4, 1, 9)} {section.counts()}")    // out: 4 10 [(0, 3072), (88, 256), (10, 512), (9, 256)]

    // As a chunk packet sends it: the bits, the palette as added, then the longs.
    var sent: List<u8> = []
    section.write(mut sent)
    println("{sent.len()} {sent[0]} {sent[1]} {sent[2]} {sent[3]}")    // out: 2054 4 4 0 88

    // As a region file saves it: the palette in the order the values first appear.
    let saved = section.pack()
    println("{saved.palette}")    // out: [88, 10, 9, 0]
    if let longs = saved.data {
        println("{longs.len()} longs")    // out: 256 longs
    }
    let loaded = PalettedContainer.unpack(blocks, saved)!
    println("{loaded.get(4, 3, 9)}")    // out: 9

    match PalettedContainer.unpack(blocks, Packed { palette: [1, 2], data: none }) {
        ok(_) => println("loaded"),
        err(e) => println("{e}"),    // out: MissingData
    }
}
```

## Containers

| Item | Gives |
| --- | --- |
| `Strategy.block_states(states: u32) -> Strategy` | A section's block states, 16 × 16 × 16, from a registry of `states`. Panics if it's 0 |
| `Strategy.biomes(biomes: u32) -> Strategy` | A section's biomes, 4 × 4 × 4, from a registry of `biomes`. Panics if it's 0 |
| `strategy.size() -> usize`, `strategy.registry_size() -> u32` | 4,096 or 64, and the registry's size |
| `strategy.index(x: i32, y: i32, z: i32) -> usize` | Where a position is among the values: `y`, then `z`, then `x`, as Minecraft orders them. Panics unless each is inside the container |
| `PalettedContainer.new(value: u32, strategy: Strategy) -> PalettedContainer` | A container of `value` everywhere |
| `c.get(x, y, z) -> u32`, `c.get_at(i: usize) -> u32` | The value at a position, or at an index |
| `c.set(x, y, z, value: u32)`, `c.set_at(i: usize, value: u32)` | Sets a value, growing the palette if it's new |
| `c.get_and_set(x, y, z, value: u32) -> u32`, `c.get_and_set_at(i, value) -> u32` | Sets a value, and gives the one it replaced |
| `c.bits() -> i32` | How many bits each value takes now |
| `c.counts() -> List<(u32, usize)>` | Each value that's there and how many times, as Minecraft's `count` gives them: the palette's order, or by ID with no palette. Sections count their blocks and fluids with it |
| `c.write(out: mut List<u8>)` | Adds the container to `out` as `PalettedContainer.write` sends it |
| `c.pack() -> Packed` | The container as `pack` saves it |
| `PalettedContainer.unpack(strategy: Strategy, packed: own Packed) -> PalettedContainer ! Error` | The container `unpack` reads from a region file's palette and longs |
| `Packed { palette: List<u32>, data: List<i64>? }` | A saved container: the palette, and the longs, which there are none of for a palette of one entry |

Values are registry IDs, so anything that isn't below the registry's size panics at the call, as an index or position outside the container does.

How the palette grows is Minecraft's, for each strategy:

| Values in the container | Block states | Biomes |
| --- | --- | --- |
| 1 | No bits: the palette is the one value | The same |
| 2 to 16 | 4 bits, a list | 1 to 3 bits for 2 to 8, a list |
| 17 to 256 | 5 to 8 bits, a list with a hash of its entries | — |
| More | No palette: each value is its ID, in as many bits as the registry's IDs need, 16 for 26.3's 35,723 states | The same past 8, 7 bits for 26.3's 65 biomes |

- A new value goes at the end of the palette. An entry stays when nothing refers to it any more, until the palette is full. Then the values move to a palette of the next size, added in index order, so it holds only those that are there, in the order they first appear.
- `write` sends a byte of the bits, then the palette as VarInts: the one entry's ID, or the count and the IDs, or nothing with no palette. Then come the longs, 8 bytes each, big-endian, with no count before them.
- `pack` makes a new palette of the values in the order they first appear, from index 0 up. The longs use the bits a palette of that size has. With more values than a list holds, that's the fewest bits that number them all, so 300 block states take 9 bits on disk, not 16.
- `unpack` gets the palette's kind from its size. With a palette, it keeps the longs as they are, including any bits above the values in each long, so they're sent as they were loaded. With no palette, it turns the indices into IDs, and only the values are kept.

Why a saved container doesn't load, as `Error`:

| Error | When | Minecraft |
| --- | --- | --- |
| `Empty` | The palette has no entries | Loads it, and fails when a value is read |
| `MissingData` | The palette has more than one entry and there are no longs | "Missing values for non-zero storage" |
| `Length { got: usize, expected: usize }` | There aren't as many longs as the values take | "Failed to read PalettedContainer: Invalid length given for storage, got: X but expected: Y" |
| `Index { at: usize, index: u32 }` | The value at `at` is an index past the palette's end | Loads it, and fails when the value is read or sent |

A palette of one entry ignores any longs, as Minecraft does. Where an entry appears twice in a palette, the first is the one later values find; Minecraft's hash palette finds one or the other by identity hash, which changes from run to run.

## Bit storage

`BitStorage` packs values of 0 to 32 bits each into longs, as `SimpleBitStorage` does: from each long's low bits up, as many whole values as fit, so none spans two longs. Heightmaps use it too, for 256 values of 9 bits in the overworld.

| Item | Gives |
| --- | --- |
| `BitStorage.new(bits: i32, size: usize) -> BitStorage` | `size` zeros. Panics unless `bits` is 0 to 32 and `size` at most 2^26. With 0 bits, there are no longs |
| `BitStorage.from_longs(bits: i32, size: usize, longs: own List<i64>) -> BitStorage ! Error` | The values packed in `longs`, kept as they are. Fails with `Length` unless there are as many longs as the values take |
| `s.bits() -> i32`, `s.len() -> usize` | The bits each value takes, and how many there are |
| `s.get(i: usize) -> u32`, `s.set(i: usize, v: u32)`, `s.get_and_set(i, v) -> u32` | One value. Panics unless `i` is below `len()` and `v` fits in the bits |
| `s.fill(v: u32)` | Sets every value to `v`, as Minecraft's `fill`, which spreads `v` by doubling: when the bits don't divide 64, the bits above each long's last value hold the low bits of one more `v` |
| `s.values() -> List<u32>` | Every value, in order |
| `s.longs() -> List<i64>`, `s.into_longs(own s) -> List<i64>` | The longs, copied or moved out |

- Finding a value's long divides by a multiply and a shift, as Minecraft does, with no division. That's exact for indices below 2^26, hence the limit on `size`.
- Values are `u32`, so 32 bits hold up to 2^32 − 1. Minecraft checks values as an `int`, so it holds only up to 2^31 − 1 there. Nothing Minecraft stores comes near: block states take 16 bits.

## Checked against Minecraft

The rig's `palette` probe (calcite/rig/PROTOCOL.md) runs a list of operations on a container or a storage and compares every result, byte for byte, with Minecraft 26.3's own `PalettedContainer` and `SimpleBitStorage`, through the same operations. Its 129 cases come from calcite/cases/palette/gen.py:

- the flat world's bottom section;
- values set one at a time past every size where a palette changes kind, for both strategies and registries from 1 value to 35,723;
- entries overwritten until the resize drops them;
- loads of every kind of palette from longs with random bits above the values, with the longs missing or the wrong length;
- random sets over spans of values that grow, with what's sent and saved looked at between them;
- every storage width from 1 to 32 bits.

All 129 match. The [tests](../../tests/std/palette.calx) hold Minecraft's bytes and orders.

`std.palette` is written in Calx: [std/palette.calx](../../std/palette.calx).
