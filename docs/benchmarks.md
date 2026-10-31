---
title: Tick-loop benchmark
description: The fixed simulation workload Calx's speed is measured on, how it's timed, and how Calx is compared with Rust.
---

# Tick-loop benchmark

This is the benchmark behind the Phase 3 gate (G3.2): Calx must run it within about 10% of Rust on Windows and on Linux. It's fixed here, before the compiler exists (`P0.6`), so the gate can't drift to suit the result. If Calx misses, D37 says what happens next.

The workload is a small, synthetic Minecraft tick: the four kinds of work Calcite spends its time on, in the shape Calcite will do them. It isn't a server and loads no game data. Both versions, the Calx one and the Rust reference (`P3.1`), implement exactly the algorithms on this page and must print the same checksum.

## The world

- **Size:** 16 × 16 chunks, each 24 sections tall (y from -64 to 319), so 6,144 sections of 16 × 16 × 16 blocks.
- **Block states:** IDs 0 to 29,999, with 0 as air. The global palette needs 15 bits, as vanilla's does.
- **Generation:** with the [random generator](#randomness) seeded with 20261027, each section picks how many distinct states it starts with: 1 (40% of sections), 2 to 16 (40%), 17 to 256 (15%) or 257 to 1,024 (5%). Its 4,096 blocks are then filled from that many random states. Sections are generated in order x, then z, then section y, all ascending.
- **Regions:** the 16 × 16 chunks are split into 16 regions of 4 × 4 chunks, numbered in row order. Region *k* owns its chunks, their sections, and the entities in them.

## The four kinds of work

### 1. Palette containers

Each section stores its blocks in vanilla's layout (`PalettedContainer` and `SimpleBitStorage` in 26.3):

- **0 bits** when the section holds one state. Reading it allocates nothing.
- **Linear palette, 4 bits** for up to 16 states: an array searched in order.
- **Hash palette, 5 to 8 bits** for up to 256 states: an ID-to-index hash map plus an index-to-ID array.
- **Global, 15 bits**, storing state IDs directly, above 256.
- Values are packed into 64-bit words and **never span two words**, so a word holds `64 / bits` values (rounded down). Finding the word divides by `values_per_word` with vanilla's multiply-and-shift trick, not a division instruction.
- When a write adds a state that doesn't fit, the section **resizes** to the next size above, re-encoding all 4,096 values, as `PalettedContainer.onResize` does.

Per tick, for every section, in region order and then section order:

- **Random ticks:** 3 random positions (vanilla's default `randomTickSpeed`). For each, read the state and its 6 neighbors in the same section (clamped at the section edges). If the sum of the 7 IDs is divisible by 7, write `(id * 31 + 7) mod 30000` back to the position.
- **One full read** of the section in index order, summing the IDs into the checksum (as chunk sending and saving do).

### 2. Entity physics in doubles

- **Entities:** 4,096, each spawned at a random position above the world's surface, with a random velocity in each axis between -0.5 and 0.5.
- **Each tick, each entity:**
  - Move with collision against full-cube blocks (any non-air state is solid), one axis at a time in vanilla's order: y first, then x before z unless `|vx| < |vz|` (`Direction.axisStepOrder`). Clip each axis so the entity's 0.6 × 1.8 × 0.6 box stops at the first solid block, and zero that axis's velocity if it was clipped.
  - Then gravity and drag, as `LivingEntity.travel` applies them to an entity in the air: `vy = (vy - 0.08) * 0.98`, `vx *= 0.91`, `vz *= 0.91`. All three constants are `f64`.
  - An entity that moves into another region's chunk is handed to that region at the barrier (an actor message carrying the entity, see below).
- Every value is an IEEE `f64`, with no fused multiply-add and no reassociation. The checksum includes every entity's final position bits, so any difference shows.

### 3. Hash-map work

The two kinds of map Calcite uses, both doing real work:

- **Java-compatible:** each region keeps a scheduled-tick set, a Java-compatible `HashMap<i64, i32>` from packed block position (vanilla's `BlockPos.asLong`) to the tick it's due. Every random tick that writes schedules the written position 2 ticks ahead. Each tick, the region removes due entries in the map's iteration order (Java's order: bucket order after Java's hash spreading, then insertion order within a bucket) and, for each, writes `(id + 1) mod 30000` at that position. The order is part of the checksum, so the map must iterate exactly as Java's `HashMap` does, resizes included.
- **Fast native:** a map from chunk position to the chunk, looked up for every block access that crosses into a neighboring chunk, and an open-addressing map used to count entities per chunk each tick. Order isn't observable, so this uses Calx's fastest map.

### 4. Actor messages

- **Regions are actors** run by the runtime's work-stealing pool, with the lockstep barrier from D19 and D20.
- **Each tick, each region** sends 64 small messages (16 bytes of payload: an `i64` position and an `i64` value) to regions chosen at random from its own stream, plus any entity handoffs from step 2.
- **At the barrier**, messages are delivered in order of sender region ID, then send order. The receiver adds each value into a running hash in delivery order, so the order is part of the checksum.
- A handoff moves the entity's ownership. The sending region must not touch it again.

### One tick

1. **Block pass.** Every region, in parallel: handle the messages delivered at the last barrier, then the scheduled ticks that are due, then random ticks and the full read, then send its messages.
2. **Entity pass.** Every region, in parallel: entity physics. Collision reads blocks in neighboring regions, so this pass starts only when every region has finished its block pass. Otherwise a region could read blocks another region is writing.
3. **The barrier:** collect every outbox, deliver in order, apply handoffs.

## Randomness

Every random choice uses Java's `java.util.Random` algorithm (a 48-bit LCG, `nextInt(bound)` with its rejection loop, `nextDouble()` from two draws), the same one Calcite uses for vanilla parity.

- **Generation:** one generator seeded with 20261027, used in the order the world section above gives.
- **Ticking:** region *k* gets its own generator seeded with `20261027 + k`. It's drawn in the order of one tick above.

So the result doesn't depend on thread count.

## The checksum

A 64-bit FNV-1a hash, starting from its standard offset basis, over:

- each tick's section sums, in order;
- the order scheduled ticks fire, as packed positions;
- each region's message hash at each barrier;
- and at the end, every section's 4,096 state IDs and every entity's position as raw `f64` bits, sorted by entity ID.

The Calx and Rust versions must print the same checksum, with any thread count. A run with a different checksum doesn't count.

## How it's measured

| What | Rule |
| --- | --- |
| Ticks | 200 warmup ticks, then 1,000 measured ticks |
| Runs | 10 per configuration, each in a fresh process |
| Metric | Mean time per measured tick in each run. The result is the median of the 10 runs. p99 tick time is reported too, but isn't gated |
| Configurations | 1 worker thread, and one worker per physical core |
| Platforms | Windows 11 and Linux (Ubuntu under WSL2) on the same machine |
| Machine | One fixed machine for every recorded number (D34), with its CPU, memory and OS build recorded beside the results. CI machines are too noisy |
| Gate | Calx's result is at most 1.10 × Rust's in every configuration on both platforms. That's the floor: the target is Calx faster than Rust in every configuration (D89) |

## Making the comparison fair

- **Same algorithms.** The Rust version follows this page as closely as the Calx one does, including a hand-written Java-compatible `HashMap` (Rust's standard map iterates in a different order) and Java's `Random`. Calx's map is its built-in [`JavaHashMap`](reference/java-maps.md) (D105), which keeps the same rules.
- **Idiomatic, safe Rust.** No `unsafe`, bounds checks left in, `std` threads or a small work-stealing pool equivalent to Calx's runtime, and wrapping arithmetic where Java wraps.
- **Same optimization level.** Rust: `--release` with `lto = "thin"`, `codegen-units = 1` and `panic = "abort"`. Calx: its release mode, clang `-O3` on the emitted C, `-ffp-contract=off`. (This said `-O2` until D89. Rust's `--release` is `opt-level = 3`, so `-O3` is the same level, and `-O2` had held Calx a level below.) Both target `x86-64-v3`, so neither gets wider instructions than the other.
- **Same toolchain generation.** The clang Calx uses and the LLVM inside rustc are both recorded. If they're far apart, that's noted with the result.

## Precise rules

The sections above leave some details open. These rules settle them, so the two versions do exactly the same work. They were fixed with the Rust reference (`P3.1`), before any Calx code existed, and they don't change the size of the workload. The one structural change is the entity pass's own step in [one tick](#one-tick).

### Layout

- Chunks are `(cx, cz)` with both from 0 to 15. Block `(x, y, z)` is in chunk `(x >> 4, z >> 4)` and section `(y + 64) >> 4`, at index `(ly << 8) | (lz << 4) | lx` within it, where `lx`, `ly` and `lz` are the low 4 bits.
- Region *k* is `(cz / 4) * 4 + cx / 4`. A region lists its chunks in x-then-z order, so chunk `(cx, cz)` is its number `(cx % 4) * 4 + cz % 4`, and each chunk lists its sections from the bottom. "Region order, then section order" means that order.
- Positions pack as vanilla's `BlockPos.asLong` does: x in bits 38 to 63, z in bits 12 to 37, y in bits 0 to 11. Chunk positions pack as `ChunkPos.asLong`: x in the low 32 bits, z in the high 32.

### Generation

- For each section: `roll = nextInt(100)`. Below 40 the section has 1 state, below 80 it has `2 + nextInt(15)`, below 95 `17 + nextInt(240)`, and otherwise `257 + nextInt(768)`.
- Its states are drawn with `nextInt(30000)`, skipping any already drawn, until there are that many. They form the palette in draw order.
- The palette starts as the smallest layout that holds them: single, linear (4 bits) up to 16, hash with `max(5, ceil(log2 n))` bits up to 256, and global otherwise.
- Then for each index from 0 to 4,095: `j = nextInt(n)`, drawn even when `n` is 1, and the index stores palette entry `j` (its state ID, in the global layout).
- After every section, the entities, by ID from 0 to 4,095: `x = 0.3 + nextDouble() * 255.4`, `y = 320 + nextDouble() * 64`, `z = 0.3 + nextDouble() * 255.4`, then `vx`, `vy` and `vz`, each `nextDouble() - 0.5`. An entity starts in the region of the chunk it's in.
- Nearly every block generated is solid, so the world's surface is its top. The entities fall onto y = 320 and come to rest there.

### Palettes

- **Bit storage.** A word holds `per_word = 64 / bits` values. Index `i` is in word `(i * m) >> 32`, with `m = floor(2^32 / per_word) + 1` and the multiply done in 64 bits. That equals `i / per_word` for every index below 4,096. The value starts at bit `(i - word * per_word) * bits`.
- **The hash palette's map** has `2^(bits + 1)` slots. A state's home slot is `(id * 0x9E3779B9 mod 2^32) >> (32 - (bits + 1))`, with linear probing.
- **Adding a state.** A single section becomes linear with palette `[old, new]`. A full linear palette (16 states) becomes a 5-bit hash palette. A full hash palette (`2^bits` states) grows by one bit, and past 8 bits becomes global.
- **Resizing** builds the new palette from the old one's entries in order, then the new state. It reads every index from 0 to 4,095 from the old storage and writes it into the new one: the same palette index, or the state's ID for the global layout.
- **The full read** is `get(i)` for every index from 0 to 4,095, in order, with the same `get` the rest of the benchmark uses.

Over the full run, the palettes stay mixed. They start as 2,418 single, 2,451 linear, 990 hash and 285 global sections, and end as 0 single, 2,828 linear, 2,287 hash and 1,029 global.

### Block pass

1. **Messages.** For each message delivered at the last barrier, in delivery order, the region's message hash takes its position, then its value.
2. **Scheduled ticks.** The region lists the keys whose due tick is at most the current one, in the map's iteration order. For each, in that order, it removes the entry, writes `(id + 1) mod 30000` at the position, and adds the packed position to its tick hash.
3. **Random ticks and the full read**, section by section. Each random tick is `i = nextInt(4096)`. A neighbor past a section face is clamped to the face, so that block counts again. If the 7 IDs sum to a multiple of 7, the tick writes `(id * 31 + 7) mod 30000`. It then puts the packed position into the scheduled-tick map, due at `tick + 2`. A `put` on a key that's already there replaces its due tick and keeps its place, as Java's does. After its 3 random ticks, the section's full-read sum goes into the tick hash.
4. **Messages out.** 64 times: `dest = nextInt(16)`, `pos = nextLong()`, `value = nextLong()`. A region can send to itself.

The scheduled-tick map is Java's `HashMap<Long, Integer>`:

- `Long.hashCode`, spread with `h ^ (h >>> 16)`.
- A 16-bucket table made on the first `put`, doubled when the size passes 0.75 of the table.
- New entries at the tail of their bucket, and resizes that split each bucket in order.
- A bucket that reaches 9 entries on a table under 64 buckets makes the table resize, as Java's `treeifyBin` does. On a bigger table Java would turn it into a tree bin. This workload never makes one, and both versions stop with an error if it does.

### Entity pass

For each of the region's entities, in list order:

1. **Its chunk** (the one its position is in) is looked up once in the chunk map. Blocks in that chunk are read from it directly. Blocks in any other chunk are found through the chunk map each time.
2. **Its box** is `[x - 0.3, x + 0.3] × [y, y + 1.8] × [z - 0.3, z + 0.3]`, computed again before each axis.
3. **Moving along one axis** by `d`, with `EPS = 1e-7`:
   - The blocks the box covers on the other two axes run from `floor(min + EPS)` to `floor(max - EPS)` on each.
   - For `d < 0`, the layers along the axis run from `floor(min + EPS) - 1` down to `floor(min + d + EPS)`. The first layer with a solid block in it limits the move to `max(d, layer + 1 - min)`.
   - For `d > 0`, the layers run from `floor(max - EPS) + 1` up to `floor(max + d - EPS)`. The first solid layer limits it to `min(d, layer - max)`.
   - The position moves by the result. If it isn't `d`, that axis's velocity becomes 0.
4. **Solid blocks.** A block is solid if its x or z is outside 0 to 255 (the border is a wall at every height) or its y is below -64. Otherwise it's air at y 320 and above. Below that it's solid unless its state is 0.
5. **Then** gravity and drag. If the entity's chunk is now in another region, it leaves the list (the rest keep their order) and is handed off.

Then the region clears its native per-chunk count map and counts its remaining entities by chunk. The sum of the squared counts goes into its tick hash.

### Barrier and checksum

- For each sender region in ID order, its messages go to the end of their receivers' inboxes. Its handoffs go to the end of their receivers' entity lists. Both keep the order they were sent in.
- Then the checksum takes each region's tick hash and then its message hash, region by region.
  - A tick hash covers one tick: the fired positions, section sums and count sum, in the order they happened.
  - A message hash runs for the whole benchmark.
- Every hash is 64-bit FNV-1a over 64-bit values, each fed as 8 little-endian bytes.
- After the last tick, the checksum takes every section's 4,096 state IDs. Sections go in generation order (x, then z, then section y) and indices in order. Then come each entity's `x`, `y` and `z` bits, by entity ID.
- The full run's checksum is in [`bench/expected.json`](../bench/expected.json): `0a7e97a835579c25`.
- A tick's time runs from the start of its block pass to the end of its barrier. p99 is the time at rank `ceil(0.99 * n)` of the `n` measured ticks.

## Files

The benchmark lives in [`bench/`](../bench/README.md) (`P3.1`, `P3.11`):

| Path | What it is |
| --- | --- |
| `bench/rust/` | The Rust reference: idiomatic, safe Rust, standard library only |
| `bench/calx/` | The Calx version (`P3.3`), the same algorithms in Calx |
| `bench/run.py` | Builds and runs a version, checks its checksum, and records the result |
| `bench/expected.json` | The checksum every version must print |
| `bench/results/` | Recorded results, one file per version and platform, naming the commit, the machine and the toolchain |

