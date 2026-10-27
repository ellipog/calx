---
title: Tick-loop benchmark
description: The fixed workload that decides the Phase 3 gate, how it's measured, and how Calx is compared with Rust.
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

1. Every region, in parallel: handle the messages delivered at the last barrier, then the scheduled ticks that are due, then random ticks and the full read, then entity physics.
2. The barrier: collect every outbox, deliver in order, apply handoffs.

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
| Gate | Calx's result is at most 1.10 × Rust's in every configuration on both platforms |

## Making the comparison fair

- **Same algorithms.** The Rust version follows this page as closely as the Calx one does, including a hand-written Java-compatible `HashMap` (Rust's standard map iterates in a different order) and Java's `Random`.
- **Idiomatic, safe Rust.** No `unsafe`, bounds checks left in, `std` threads or a small work-stealing pool equivalent to Calx's runtime, and wrapping arithmetic where Java wraps.
- **Same optimization level.** Rust: `--release` with `lto = "thin"`, `codegen-units = 1` and `panic = "abort"`. Calx: its release mode, clang `-O2` on the emitted C, `-ffp-contract=off`. Both target `x86-64-v3`, so neither gets wider instructions than the other.
- **Same toolchain generation.** The clang Calx uses and the LLVM inside rustc are both recorded. If they're far apart, that's noted with the result.

## Files

From Phase 3, the Calx version, the Rust reference, a runner script and the recorded results live in `bench/` in this repo (`P3.11`). Each result names the Calx commit, the machine and both toolchains.
